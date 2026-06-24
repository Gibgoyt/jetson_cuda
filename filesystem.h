/*
 * Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
*/

/*
 *	filesystem.h  --  POSIX path / file / directory helpers
 *	=======================================================
 *
 *	Position in the pipeline (Stage 1, "Pure C++ foundations" per
 *	PLAN.md -- third file in the reading order, after commandLine.{h,cpp}):
 *		Every loader in the library funnels through here. Models, label
 *		files, image inputs, calibration tables, the .engine cache --
 *		all of them ultimately call into one of these helpers to turn a
 *		user-supplied path into an absolute path, find it under a search
 *		set of system locations, slurp it into memory, or enumerate the
 *		images in a folder for the listDir-driven test apps.
 *
 *	What this file provides (six small groups, header-only declarations
 *	plus one enum):
 *
 *		Path resolution
 *			absolutePath  -- resolve a relative path against cwd
 *			locateFile    -- search common locations for a file
 *			locateFile    -- as above, plus user-supplied locations
 *
 *		Bulk file I/O
 *			loadFile      -- read binary blob into malloc'd buffer
 *			readFile      -- read text file into std::string
 *
 *		Directory enumeration
 *			listDir       -- glob a directory, alphanum-sorted, filtered
 *
 *		Stat wrappers (POSIX `stat(2)` underneath)
 *			fileType      -- single-bit fileTypes value
 *			fileIsType    -- bool, with a type mask
 *			fileExists    -- thin alias for fileIsType
 *			fileSize      -- size in bytes (0 on missing)
 *			enum fileTypes -- REGULAR / DIR / LINK / CHAR / BLOCK / FIFO / SOCKET
 *
 *		Path manipulation (pure string ops, no I/O)
 *			splitPath     -- (dir, filename) pair
 *			pathFilename  -- basename
 *			pathDir       -- dirname (with caveats below)
 *			pathJoin      -- combine two path components with one `/`
 *
 *		Extension manipulation (pure string ops, lowercased)
 *			fileExtension          -- ".jpg" -> "jpg"
 *			fileHasExtension       -- (3 overloads: single, vector, NULL-list)
 *			fileRemoveExtension    -- strip the trailing extension
 *			fileChangeExtension    -- swap one extension for another
 *
 *	Conventions across the API:
 *		- All paths are std::string. No std::filesystem here -- the
 *		  upstream targets predate C++17 std::filesystem being available
 *		  on Jetson L4T toolchains.
 *		- Failure is reported in-band: empty string (path getters),
 *		  0 (sizes), false (predicates). Only loadFile and listDir log
 *		  the failure via LogError.
 *		- Path separators are POSIX-style `/`. A few functions also
 *		  recognise `\` as a separator (absolutePath, pathJoin) for
 *		  resilience against Windows-style input, but the file is not
 *		  expected to compile on Windows.
 *
 *	Gotchas that affect callers (all kept as inline NOTE: comments
 *	in the .cpp where each issue lives):
 *		1. absolutePath DOES NOT expand `~`. A path that begins with
 *		   `~` is returned verbatim; downstream code that passes the
 *		   result to a non-shell consumer (fopen, stat, ...) will fail
 *		   to find the file.
 *		2. splitPath has surprising semantics for paths with no slash:
 *		   `splitPath("foo")`     -> ("foo", "")   // treated as a dir
 *		   `splitPath("foo.txt")` -> ("", "foo.txt") // treated as file
 *		3. pathDir returns the INPUT unchanged when there is no slash,
 *		   and ALSO when the only slash is at index 0
 *		   (`pathDir("/foo")` -> "/foo", not "/").
 *		4. loadFile leaks the buffer when called with a NULL bufferOut
 *		   (it allocates, reads, then forgets to either expose or free
 *		   the result). Always pass a real out-pointer.
 *		5. fileIsType uses a subset-test on the mask, not an
 *		   intersection -- the file's single type bit must be contained
 *		   in `mask`. Works out OK because fileType always returns a
 *		   single bit, but counter-intuitive if you read the body.
 *		6. fileExtension lowercases via the unqualified `tolower`,
 *		   which is the C `int(int)` overload. Undefined behaviour for
 *		   non-ASCII (negative `char`) bytes -- e.g. UTF-8 paths.
 *		7. listDir glob() falls back to retrying with the executable
 *		   directory prepended on GLOB_NOMATCH, if the path doesn't
 *		   start with `.`, `/`, `\`, `*`, `?`, or `~`. Convenient for
 *		   bundled `images/` shipping next to the binary, but surprising
 *		   if you don't know to expect it.
 *		8. The doxygen block on the line before fileExists below is
 *		   visibly malformed in the upstream source -- an unterminated
 *		   `/** Return the directory` is followed by another `/**`
 *		   which the preprocessor treats as plain text. The result is
 *		   one fat comment attached to fileExists. Kept verbatim so the
 *		   generated docs match upstream; flagged here so future readers
 *		   know not to "tidy" it without checking what was intended.
 *
 *	Cross-references:
 *		- Process.h  -- GetWorkingDir / GetExecutableDir (used by
 *		  absolutePath, locateFile, listDir).
 *		- alphanum.h -- natural-order sort used inside listDir so
 *		  "img2" comes before "img10".
 *		- logging.h  -- LogError / LogWarning paths used on I/O failure.
*/

#if !defined(__FILESYSTEM_UTIL_H__)
	#define __FILESYSTEM_UTIL_H__

	#include <string>
	#include <vector>

	/*
	 *	------------------------------------------------------------------
	 *	Path resolution
	 *	------------------------------------------------------------------
	*/

	/**
	 * Given a relative path, resolve the absolute path using the working directory.
	 *
	 * For example, if the current working directory `/home/user/` and
	 * `absolutePath("resources/example")` is called, then this function
	 * would return the path `/home/user/resources/example`.
	 *
	 * If the path is already an absolute path (i.e. it begins with `/` or `~/`)
	 * then this function will be ignored and the path will be returned as-is.
	 *
	 * @ingroup filesystem
	 *
	 *	NOTE: `~` is treated as "already absolute" and returned verbatim
	 *	-- it is NOT expanded to $HOME. If the result is handed to fopen
	 *	or stat directly (no shell in between) it will fail to resolve.
	*/
	std::string absolutePath(
	    const std::string& relative_path
	);

	/**
	 * Locate a file from common system locations.
	 * First, this function will check if the file exists at the path provided,
	 * and if not it will check for the existance of the file in common system
	 * locations such as "/opt", "/usr/local", and "/usr/local/bin".
	 *
	 * @return the confirmed path of the located file, or empty string if
	 *         the file could not be found
	 *
	 * @ingroup filesystem
	 *
	 *	Built-in search order (after the literal path): the executable's
	 *	directory, /usr/local/bin/, /usr/local/, /opt/, images/,
	 *	/usr/local/bin/images/. The two `images/` paths exist because the
	 *	library ships a small bundle of test JPEGs the examples reference
	 *	by bare name.
	*/
	std::string locateFile(
	    const std::string& path
	);

	/**
	 * Locate a file from a set of locations provided by the user, in addition
	 * to common system locations such as "/opt" and "/usr/local".
	 *
	 * @return the confirmed path of the located file, or empty string if
	 *         the file could not be found
	 *
	 * @ingroup filesystem
	 *
	 *	The `locations` vector is treated as in/out: callers can pass in
	 *	additional search locations, and on return the vector also holds
	 *	the built-in fallbacks that were appended during the search. If
	 *	you don't want that side effect, copy before calling.
	*/
	std::string locateFile(
	    const std::string& path,
	    std::vector<std::string>& locations
	);

	/*
	 *	------------------------------------------------------------------
	 *	Bulk file I/O
	 *	------------------------------------------------------------------
	*/

	/**
	 * Loads a binary file into a buffer that it allocates.
	 * @return the size in bytes read (or 0 on error)
	 * @ingroup filesystem
	 *
	 *	NOTE: caller takes ownership of *buffer and must free() it.
	 *	NOTE: do NOT pass a NULL `buffer` out-pointer. The implementation
	 *	      allocates first and only assigns through `buffer` after a
	 *	      NULL-check, so a NULL out-pointer leaks the malloc'd block.
	*/
	size_t loadFile(
	    const std::string& path,
	    void** buffer
	);

	/**
	 * Read a text file into a string.  It's assumed that the file is text, and that it is of a
	 * managegable size (otherwise you should use buffering and read it line-by-line)
	 *
	 * @return the string containing the file contents, or an empty string if an error occurred.
	 *
	 * @ingroup filesystem
	 *
	 *	Opens the file in binary mode (`std::ios::in | std::ios::binary`),
	 *	so newline translation is NOT applied -- you get CRLF if the file
	 *	has it. An empty file returns "" and logs a LogWarning (NOT an
	 *	error), so the only way to distinguish empty-file from open-fail
	 *	without scanning the log is to stat the file first.
	*/
	std::string readFile(
	    const std::string& path
	);

	/*
	 *	------------------------------------------------------------------
	 *	Path manipulation (pure string ops -- no I/O)
	 *	------------------------------------------------------------------
	*/

	/**
	 * Join two paths, and properly include a path separator (`/`) as needed.
	 * For example, 'pathJoin("~/workspace", "somefile.xml")` would return `~/workspace/somefile.xml`.
	 * @ingroup filesystem
	 *
	 *	A trailing `/` or `\` on `a` is detected and reused as the
	 *	separator (so paths picked up from Windows-style input pass
	 *	through unchanged); otherwise a `/` is inserted. Empty operands
	 *	short-circuit to the non-empty side.
	*/
	std::string pathJoin(
	    const std::string& a,
	    const std::string& b
	);

	/**
	 * Return the parent directory of the specified path, removing the filename and extension.
	 * For example, `pathDir("~/workspace/somefile.xml")` would return `~/workspace/`
	 * @ingroup filesystem
	 *
	 *	NOTE: returns the INPUT unchanged when there is no slash, AND
	 *	when the only slash is at index 0 -- so `pathDir("foo")` -> "foo"
	 *	(not "") and `pathDir("/foo")` -> "/foo" (not "/"). Callers that
	 *	need a clean "directory of a top-level entry" must special-case
	 *	these.
	*/
	std::string pathDir(
	    const std::string& path
	);

	/**
	 * Return the filename from the path, including the file extension.
	 * @ingroup filesystem
	 *
	 *	When the path has no `/`, returns the path verbatim (the whole
	 *	thing IS the filename). When the path ends with `/`, returns the
	 *	empty string.
	*/
	std::string pathFilename(
	    const std::string& path
	);

	/**
	 * Split a path into directory and filename components.
	 * The directory will be returned first in the pair, and the filename second.
	 * @ingroup filesystem
	 *
	 *	NOTE: for a path WITHOUT a slash, the split decision is based on
	 *	whether the path has an extension:
	 *	  splitPath("foo")     -> ("foo", "")    // treated as a directory
	 *	  splitPath("foo.txt") -> ("", "foo.txt") // treated as a filename
	 *	If you can't guarantee the input shape, prefer pathDir +
	 *	pathFilename, which have independent definitions.
	*/
	std::pair<std::string, std::string> splitPath(
	    const std::string& path
	);

	/*
	 *	------------------------------------------------------------------
	 *	File types (bitmask used by listDir / fileIsType / fileExists)
	 *	------------------------------------------------------------------
	*/

	/**
	 * File types
	 * @ingroup filesystem
	 *
	 *	Mirrors the POSIX S_IS{REG,DIR,LNK,CHR,BLK,FIFO,SOCK} predicates.
	 *	Designed to be OR'd together when passed as a `mask` parameter:
	 *	  fileExists(p, FILE_REGULAR | FILE_DIR)
	 *	  listDir(p, out, FILE_REGULAR)
	 *	FILE_MISSING == 0 is also used as the "any file type" mask value,
	 *	per the early-return in fileIsType.
	*/
	enum fileTypes {
		FILE_MISSING = 0,
		FILE_REGULAR = (1 << 0),
		FILE_DIR = (1 << 1),
		FILE_LINK = (1 << 2),
		FILE_CHAR = (1 << 3),
		FILE_BLOCK = (1 << 4),
		FILE_FIFO = (1 << 5),
		FILE_SOCKET = (1 << 6)
	};

	/*
	 *	------------------------------------------------------------------
	 *	Directory enumeration
	 *	------------------------------------------------------------------
	*/

	/**
	 * Return a sorted list of the files in the specified directory.  listDir() will glob files from
	 * the specified path, and filter against wildcard characters including `*` and `?`.
	 * For example, valid paths would include `~/workspace`, `~/workspace/*.jpg`, ect.
	 *
	 * @see here for a description of wildcard matching:
	 * https://www.man7.org/linux/man-pages/man7/glob.7.html
	 *
	 * @param path the path of the directory (may include wildcard characters)
	 * @param[out] list the alphanumerically sorted output list of the files in the directory
	 * @param mask filter by file type (by default, any file including directories will be included).
	 *             The mask should consist of fileTypes OR'd together (e.g. `FILE_REGULAR|FILE_DIR`).
	 *
	 * @ingroup filesystem
	 *
	 *	If `path` resolves to a directory, `/*` is appended before
	 *	globbing so the directory contents are listed (rather than just
	 *	the directory entry itself). Globs use GLOB_PERIOD | GLOB_MARK |
	 *	GLOB_BRACE | GLOB_TILDE_CHECK -- so `.dotfiles` ARE matched, trailing
	 *	`/` is added to directory entries, `{jpg,png}` brace expansion
	 *	works, and a stray `~` that doesn't resolve is an error rather
	 *	than literal.
	 *
	 *	NOTE: on GLOB_NOMATCH, if the path doesn't start with `.`, `/`,
	 *	`\`, `*`, `?`, or `~`, listDir retries once with the executable
	 *	directory prepended. Useful for bundled `images/` -- surprising
	 *	if you weren't expecting it.
	 *
	 *	Output is alphanum-sorted ("img2" before "img10") via alphanum.h.
	*/
	bool listDir(
	    const std::string& path,
	    std::vector<std::string>& list,
	    uint32_t mask = 0
	);

	/*
	 *	------------------------------------------------------------------
	 *	stat(2) wrappers
	 *	------------------------------------------------------------------
	*/

	/**
	 * Return the directory
	/**
	 * Verify path and return true if the file exists.
	 * @param mask filter by file type (by default, any file including directories will be checked).
	 *             The mask should consist of fileTypes OR'd together (e.g. `FILE_REGULAR|FILE_DIR`).
	 * @ingroup filesystem
	 *
	 *	NOTE: the comment block above is malformed in upstream source --
	 *	the `/** Return the directory` line opens a doxygen block that is
	 *	never closed, and the next `/**` is parsed as text rather than a
	 *	new comment. The orphan "Return the directory" text was probably
	 *	intended for a separate pathDir block (already declared earlier).
	 *	Kept verbatim to match upstream; do not "tidy" without checking
	 *	what was intended.
	 *
	 *	Behaviour: returns true iff stat(2) succeeds AND (mask == 0 OR
	 *	the file's type bit is contained in mask). Use FILE_REGULAR if
	 *	you specifically want to exclude directories from "exists".
	*/
	bool fileExists(
	    const std::string& path,
	    uint32_t mask = 0
	);

	/**
	 * Return true if the file is one of the types in the fileTypes mask.
	 * @param mask file types to check against (@see fileTypes)
	 *             The mask should consist of fileTypes OR'd together (e.g. `FILE_REGULAR|FILE_DIR`).
	 * @ingroup filesystem
	 *
	 *	NOTE: implemented as a SUBSET test, not an intersection -- the
	 *	check is `(type & mask) != type`, which requires the file's type
	 *	bit to be present in mask. Works fine because fileType returns
	 *	exactly one bit, but the idiom is unusual.
	*/
	bool fileIsType(
	    const std::string& path,
	    uint32_t mask
	);

	/**
	 * Return the file type, or FILE_MISSING if it doesn't exist.
	 * @see fileTypes
	 * @ingroup filesystem
	 *
	 *	Always returns a SINGLE bit from the fileTypes enum, never a
	 *	combination. FILE_MISSING (== 0) is returned both for "not found"
	 *	and for an unrecognized stat() st_mode (the latter shouldn't
	 *	happen on Linux).
	*/
	uint32_t fileType(
	    const std::string& path
	);

	/**
	 * Return the size (in bytes) of the specified file.
	 *
	 * @param path the path of the file
	 * @return if successful, the size of the file in bytes
	 *         otherwise, 0 will be returned.
	 *
	 * @ingroup filesystem
	 *
	 *	A genuinely-empty regular file also returns 0 -- this overload
	 *	can't distinguish "missing" from "empty". Use fileExists first
	 *	if you need to.
	*/
	size_t fileSize(
	    const std::string& path
	);

	/*
	 *	------------------------------------------------------------------
	 *	Extension manipulation (pure string ops, lowercased on read)
	 *	------------------------------------------------------------------
	*/

	/**
	 * Extract the file extension from the path.
	 * This function will return all contents of the path to the right of the right-most `'.'`
	 * The extension will be returned in all lowercase characters.
	 * @ingroup filesystem
	 *
	 *	NOTE: lowercasing is done via the unqualified `tolower(int)`,
	 *	which is undefined behaviour for `char` values that sign-extend
	 *	negative -- i.e. non-ASCII bytes in UTF-8 paths. In practice
	 *	extensions are ASCII so it works, but be aware if you ever feed
	 *	this Unicode filenames.
	 *
	 *	No leading `.` in the result: "foo.JPG" -> "jpg".
	*/
	std::string fileExtension(
	    const std::string& path
	);

	/**
	 * Return true if the file has the given extension, otherwise false.
	 * For example, `fileHasExtension("~/workspace/image.jpg", "jpg")` would return true.
	 * @ingroup filesystem
	 *
	 *	Comparison is case-insensitive (strcasecmp). The expected
	 *	extension should NOT include the leading `.`.
	*/
	bool fileHasExtension(
	    const std::string& path,
	    const std::string& extension
	);

	/**
	 * Return true if the file has one of the given extensions, otherwise false.
	 * @ingroup filesystem
	*/
	bool fileHasExtension(
	    const std::string& path,
	    const std::vector<std::string>& extensions
	);

	/**
	 * Return true if the file has one of the given extensions, otherwise false.
	 * For example, `fileHasExtension("image.jpg", {"jpg", "jpeg", NULL})` would return true.
	 * @param extensions list of extensions, should end with `NULL` sentinel.
	 * @ingroup filesystem
	 *
	 *	C-string overload for callers building extension lists from
	 *	macros or static arrays. The NULL sentinel is mandatory --
	 *	without it the loop walks off the end.
	*/
	bool fileHasExtension(
	    const std::string& path,
	    const char** extensions
	);

	/**
	 * Return the input string with the file extension removed
	 * For example, `fileRemoveExtension("~/workspace/somefile.xml")`
	 * would return `~/user/somefile`.
	 * @ingroup filesystem
	 *
	 *	Robust against a `.` that lives inside a directory component:
	 *	`fileRemoveExtension("~/foo.bar/baz")` returns the input
	 *	unchanged because the last `.` precedes the last `/`.
	*/
	std::string fileRemoveExtension(
	    const std::string& filename
	);

	/**
	 * Return the input string with a changed file extension
	 * For example, `fileChangeExtension("~/workspace/somefile.xml", "zip")`
	 * would return `~/user/somefile.zip`.
	 * @ingroup filesystem
	 *
	 *	Concatenation only -- does NOT insert a `.`. So callers must
	 *	include the leading dot in `newExtension`:
	 *	  fileChangeExtension("a.xml", ".zip")  -> "a.zip"
	 *	  fileChangeExtension("a.xml",  "zip")  -> "azip"
	*/
	std::string fileChangeExtension(
	    const std::string& filename,
	    const std::string& newExtension
	);

#endif
