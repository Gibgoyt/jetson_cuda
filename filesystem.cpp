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
 *	filesystem.cpp  --  implementation of the path / file / dir helpers
 *	===================================================================
 *
 *	See filesystem.h for the public API surface and the high-level
 *	gotchas. This file is the small, mostly-self-contained collection
 *	of implementations behind those declarations.
 *
 *	External dependencies:
 *		<sys/stat.h>   -- stat(2) and the S_IS{REG,DIR,...} predicates
 *		<glob.h>       -- POSIX glob(3), used by listDir
 *		<fstream>      -- std::ifstream, used by readFile
 *		alphanum.h     -- doj::alphanum_less, natural-order sort for listDir
 *		Process.h      -- GetWorkingDir / GetExecutableDir (defined elsewhere
 *		                  in utils/; wraps /proc/self/cwd and /proc/self/exe)
 *		logging.h      -- LogError / LogWarning
 *
 *	Error-reporting convention:
 *		- Path getters: empty string on failure.
 *		- Size getters: 0 on failure (also valid for an empty file --
 *		  callers can't always distinguish).
 *		- Predicates:   false on failure.
 *		- I/O entries (loadFile, listDir): log via LogError on the
 *		  failure path BEFORE returning. readFile logs only an empty-
 *		  file warning, not an open failure -- caller can't tell from
 *		  the return value alone.
*/

#include "filesystem.h"
#include "alphanum.h"
#include "Process.h"

#include <sys/stat.h>
#include <algorithm>
#include <strings.h>
#include <string>
#include <fstream>
#include <streambuf>
#include <glob.h>

#include "logging.h"


//-----------------------------------------------------------------------------------
// Path resolution
//-----------------------------------------------------------------------------------

// absolutePath
//
// Test the first byte: `/` is POSIX-absolute, `\` is Windows-absolute
// (handled defensively for cross-platform input), `~` is a home-relative
// path that the caller is expected to expand themselves (see NOTE).
std::string absolutePath(
    const std::string& relative_path
) {
	if (relative_path.size() != 0) {
		const char first_char = relative_path[0];

		// NOTE: `~` is treated as already-absolute and passed through
		// unchanged. This function does NOT expand it -- callers that
		// hand the result directly to fopen/stat will fail to resolve.
		if (first_char == '/' || first_char == '\\' || first_char == '~')
			return relative_path;
	}

	return pathJoin(Process::GetWorkingDir(), relative_path);
}

// locateFile
//
// Thin convenience overload -- delegates to the (path, locations) form
// with an empty seed vector. The built-in fallback locations are
// appended inside the two-arg overload below.
std::string locateFile(
    const std::string& path
) {
	std::vector<std::string> locations;
	return locateFile(path, locations);
}

// locateFile
//
// In/out: callers can pre-populate `locations` to extend the search
// set. On return the vector also contains the built-in fallbacks
// appended below. Search stops at the first match.
std::string locateFile(
    const std::string& path,
    std::vector<std::string>& locations
) {
	// check the given path first
	if (fileExists(path.c_str()))
		return path;

	// add standard search locations
	//
	// Order matters: a path that exists in multiple locations resolves
	// to the EARLIER one. The executable directory wins over /usr/local
	// because dev builds typically run in-tree and find their own
	// bundled assets before the system install.
	locations.push_back(Process::GetExecutableDir());

	locations.push_back("/usr/local/bin/");
	locations.push_back("/usr/local/");
	locations.push_back("/opt/");

	// The two `images/` paths cover the bundled-test-images case --
	// the library ships ~30 small JPEGs that examples reference by
	// bare name (`peds-001.jpg` etc.).
	locations.push_back("images/");
	locations.push_back("/usr/local/bin/images/");

	// check each location until the file is found
	const size_t numLocations = locations.size();

	for (size_t n = 0; n < numLocations; n++) {
		const std::string str = pathJoin(locations[n], path);

		if (fileExists(str.c_str()))
			return str;
	}

	return "";
}


//-----------------------------------------------------------------------------------
// Bulk file I/O
//-----------------------------------------------------------------------------------

// loadFile
//
// Stat-then-malloc-then-fread pattern. Returns the byte count on
// success and 0 on any failure (allocation, open, short read).
//
// NOTE: if `bufferOut == NULL`, the malloc'd buffer is leaked AND
//       the function still returns bytes_read. The NULL-check is in
//       the wrong place -- the buffer should either be free()'d in
//       that branch or the NULL-check should happen up front. Behaviour
//       preserved here, called out so callers don't pass NULL.
size_t loadFile(
    const std::string& path,
    void** bufferOut
) {
	// determine the file size
	const size_t file_size = fileSize(path);

	if (file_size == 0)
		return 0;

	// allocate memory to hold the file
	void* buffer = (void*)malloc(file_size);

	if (!buffer) {
		LogError("failed to allocate %zu bytes to read %s\n", file_size, path.c_str());
		return 0;
	}

	// read the file
	FILE* file = fopen(path.c_str(), "rb");

	if (!file) {
		LogError("failed to open %s\n", path.c_str());
		free(buffer);
		return 0;
	}

	// read the serialized engine into memory
	//
	// The comment in upstream calls this an "engine" because the
	// dominant caller is tensorNet loading a serialized .engine file
	// (see c/tensorNet.cpp). Generic enough to be used for any blob.
	const size_t bytes_read = fread(buffer, 1, file_size, file);

	if (bytes_read != file_size) {
		LogError("only read %zu of %zu bytes from %s\n", bytes_read, file_size, path.c_str());
		free(buffer);
		return 0;
	}

	fclose(file);

	// NOTE: if bufferOut == NULL we leak `buffer` here. The caller
	// has no way to recover the data and no way to free it. Callers
	// must pass a valid out-pointer.
	if (bufferOut != NULL)
		*bufferOut = buffer;

	return bytes_read;
}

// readFile
//
// Reference: https://insanecoding.blogspot.com/2011/11/how-to-read-in-file-in-c.html
//
// Idiomatic streambuf_iterator slurp. Binary mode means no newline
// translation -- the returned string is a byte-for-byte copy of the
// file. An empty file is NOT distinguishable from a missing file at
// the return-value level (both return ""), only via the LogWarning
// vs LogError split in the message stream.
std::string readFile(
    const std::string& path
) {
	std::ifstream in(path, std::ios::in | std::ios::binary);

	if (!in) {
		LogError("failed to find/open file %s\n", path.c_str());
		return std::string();
	}

	const std::string contents(
	    (std::istreambuf_iterator<char>(in)),
	    std::istreambuf_iterator<char>()
	);

	if (contents.length() == 0) {
		LogWarning("file was empty - %s\n", path.c_str());
	}

	return contents;
}


//-----------------------------------------------------------------------------------
// Directory enumeration
//-----------------------------------------------------------------------------------

// listDir
//
// Wraps POSIX glob(3) with two QoL behaviours:
//   1. If `path_in` is a directory, append `/*` so the contents are
//      listed instead of just the directory entry itself.
//   2. On GLOB_NOMATCH for a path that doesn't start with a "rooted"
//      character (`.`, `/`, `\`, `*`, `?`, `~`), retry once with the
//      executable directory prepended -- this is how bundled
//      images/ next to the binary just works.
//
// Glob flag rationale:
//   GLOB_PERIOD       -- match leading `.` filenames
//   GLOB_MARK         -- append `/` to directory matches
//   GLOB_BRACE        -- expand `{jpg,png}` style alternations
//   GLOB_TILDE_CHECK  -- expand `~`, error if it doesn't resolve
//                        (rather than treating as a literal `~`)
//
// Output is filtered by `mask` (see fileTypes) and then alphanum-sorted
// so "img2" precedes "img10".
bool listDir(
    const std::string& path_in,
    std::vector<std::string>& output,
    uint32_t mask
) {
	std::string path = path_in;

	if (path.size() == 0)
		return false;

	// add a wildcard under directories, otherwise just the dir will be returned
	const bool pathIsDir = fileIsType(path, FILE_DIR | FILE_LINK);

	if (pathIsDir)
		path = pathJoin(path, "*");

	// glob the files - https://www.man7.org/linux/man-pages/man3/glob.3.html
	glob_t globList;

	const int result = glob(
	    path.c_str(),
	    GLOB_PERIOD | GLOB_MARK | GLOB_BRACE | GLOB_TILDE_CHECK,
	    NULL,
	    &globList
	);

	if (result != 0) {
		if (result == GLOB_NOSPACE) {
			LogError("listDir('%s') - ran out of memory\n", path.c_str());
		} else if (result == GLOB_ABORTED) {
			LogError("listDir('%s') - aborted due to read error or permissions\n", path.c_str());
		} else if (result == GLOB_NOMATCH) {
			const char firstChar = path[0];

			// if nothing was found and a full path wasn't specified, try the exe path
			//
			// Skip the retry for paths that already look "rooted":
			// relative-to-cwd (`.`), absolute (`/`, `\`), glob-anchored
			// (`*`, `?`), or home-relative (`~`). Anything else gets
			// one fallback shot at exe-dir/path.
			if (firstChar != '.' && firstChar != '/' && firstChar != '\\' && firstChar != '*' &&
			    firstChar != '?' && firstChar != '~')
				return listDir(pathJoin(Process::GetExecutableDir(), path), output, mask);
			else
				LogError("listDir('%s') - found no matches\n", path.c_str());
		}

		return false;
	}

	// populate the output vector, and filter by file type
	for (size_t n = 0; n < globList.gl_pathc; n++) {
		// if there's a type mask, check that it matches
		if (mask != 0 && !fileIsType(globList.gl_pathv[n], mask))
			continue;

		output.push_back(globList.gl_pathv[n]);
	}

	globfree(&globList);

	// sort list alphanumerically (glob actually already does this)
	//
	// glob() sorts lexically, which puts "img10" before "img2". The
	// alphanum_less comparator from alphanum.h sorts numeric runs
	// numerically, giving the human-expected order.
	std::sort(output.begin(), output.end(), doj::alphanum_less<std::string>());

	if (output.size() == 0) {
		LogError("%s didn't match any files\n", path.c_str());
		return false;
	}

	return true;
}


//-----------------------------------------------------------------------------------
// stat(2) wrappers
//-----------------------------------------------------------------------------------

// fileType
//
// Single stat(2) call; map st_mode to one of the fileTypes bits. Returns
// FILE_MISSING (== 0) for both "stat failed" and "stat succeeded but
// returned an unrecognized mode" -- the latter is unreachable on Linux.
uint32_t fileType(
    const std::string& path
) {
	if (path.size() == 0)
		return FILE_MISSING;

	struct stat fileStat;
	const int result = stat(path.c_str(), &fileStat);

	if (result == -1) {
		// printf("%s does not exist.\n", path.c_str());
		return FILE_MISSING;
	}

	if (S_ISREG(fileStat.st_mode))
		return FILE_REGULAR;
	else if (S_ISDIR(fileStat.st_mode))
		return FILE_DIR;
	else if (S_ISLNK(fileStat.st_mode))
		return FILE_LINK;
	else if (S_ISCHR(fileStat.st_mode))
		return FILE_CHAR;
	else if (S_ISBLK(fileStat.st_mode))
		return FILE_BLOCK;
	else if (S_ISFIFO(fileStat.st_mode))
		return FILE_FIFO;
	else if (S_ISSOCK(fileStat.st_mode))
		return FILE_SOCKET;

	return FILE_MISSING;
}

// fileIsType
//
// NOTE: the `(type & mask) != type` check is a SUBSET test --
// "the file's type bit must be contained in mask". This is unusual;
// the more common idiom would be intersection ((type & mask) != 0).
// It works out correctly here only because fileType always returns
// a single bit. If fileType were ever extended to return multiple
// bits (e.g. FILE_LINK | FILE_REGULAR for a symlink-to-file) the
// semantics would silently change.
bool fileIsType(
    const std::string& path,
    uint32_t mask
) {
	if (path.size() == 0)
		return false;

	const uint32_t type = fileType(path);

	if (type == FILE_MISSING)
		return false;

	if (mask == 0)
		return true;

	if ((type & mask) != type)
		return false;

	return true;
}

// fileExists
//
// Pure alias for fileIsType -- "exists" is "is some type, with the
// optional mask filter applied". Kept as a separate name for caller
// readability at use sites like `if (fileExists(modelPath))`.
bool fileExists(
    const std::string& path,
    uint32_t mask
) {
	return fileIsType(path, mask);
}

// fileSize
//
// Returns st_size on success, 0 on failure. A genuinely empty file also
// returns 0, so callers needing to distinguish must call fileExists
// (or fileType) first.
size_t fileSize(
    const std::string& path
) {
	if (path.size() == 0)
		return 0;

	struct stat fileStat;

	const int result = stat(path.c_str(), &fileStat);

	if (result == -1) {
		LogError("%s does not exist.\n", path.c_str());
		return 0;
	}

	// printf("%s  size %zu bytes\n", path, (size_t)fileStat.st_size);
	return fileStat.st_size;
}


//-----------------------------------------------------------------------------------
// Path manipulation (pure string ops -- no I/O)
//-----------------------------------------------------------------------------------

// splitPath
//
// NOTE: surprising semantics for paths with no slash --
//   splitPath("foo")     -> ("foo", "")    // treated as directory
//   splitPath("foo.txt") -> ("", "foo.txt") // treated as filename
// The decision hinges on whether fileExtension() returns non-empty.
// If you can't guarantee the input shape, prefer using pathDir +
// pathFilename, which have independent definitions and no such
// interaction.
std::pair<std::string, std::string> splitPath(
    const std::string& path
) {
	const std::string::size_type slashIdx = path.find_last_of("/");
	const std::string ext = fileExtension(path);

	if (slashIdx == std::string::npos) {
		if (ext.length() > 0)
			return std::pair<std::string, std::string>("", path);
		else
			return std::pair<std::string, std::string>(path, "");
	}

	return std::pair<std::string, std::string>(
	    path.substr(0, slashIdx + 1),
	    path.substr(slashIdx + 1)
	);
}

// pathFilename
//
// Basename. No slash -> the whole input is the filename. Trailing
// slash -> empty string (the part after the last `/` is "").
std::string pathFilename(
    const std::string& path
) {
	const std::string::size_type slashIdx = path.find_last_of("/");

	if (slashIdx == std::string::npos)
		return path;

	return path.substr(slashIdx + 1);
}

// pathDir
//
// NOTE: returns the INPUT unchanged in two edge cases --
//   - no slash at all   (e.g. "foo"  -> "foo")
//   - only slash at idx 0 (e.g. "/foo" -> "/foo", not "/")
// Both are arguably bugs (the docstring promises "parent directory")
// but downstream code may depend on the no-slash case as a "if there's
// no directory, the path IS the directory" tell. Don't fix without
// auditing callers.
std::string pathDir(
    const std::string& path
) {
	const std::string::size_type slashIdx = path.find_last_of("/");

	if (slashIdx == std::string::npos || slashIdx == 0)
		return path;

	return path.substr(0, slashIdx + 1);
}

// pathJoin
//
// Empty operands short-circuit. Otherwise, reuse a trailing `/` or `\`
// on `a` as the separator (preserves Windows-style separators in
// transit); otherwise insert a `/`. No normalization of duplicate
// slashes or `..` -- this is concatenation, not canonicalization.
std::string pathJoin(
    const std::string& a,
    const std::string& b
) {
	if (a.size() == 0)
		return b;

	if (b.size() == 0)
		return a;

	// check if there is already a path separator at the end
	const char lastChar = a[a.size() - 1];

	if (lastChar == '/' || lastChar == '\\')
		return a + b;

	return a + "/" + b;
}


//-----------------------------------------------------------------------------------
// Extension manipulation (pure string ops, lowercased on read)
//-----------------------------------------------------------------------------------

// fileExtension
//
// NOTE: `tolower` here is the unqualified C function `int(int)`. For
// bytes that sign-extend to negative `int` (non-ASCII in UTF-8) this
// is undefined behaviour. Safe in practice because extensions are
// ASCII, but worth knowing if you ever feed this Unicode paths.
//
// No leading `.` in the returned string: "foo.JPG" -> "jpg". A path
// with a `.` only in a directory segment (e.g. "foo.bar/baz") will
// confuse this -- find_last_of('.') returns the dir-segment dot. Use
// fileRemoveExtension's guard if you care.
std::string fileExtension(
    const std::string& path
) {
	const std::string::size_type dotIdx = path.find_last_of(".");

	if (dotIdx == std::string::npos)
		return "";

	std::string ext = path.substr(dotIdx + 1);
	transform(ext.begin(), ext.end(), ext.begin(), tolower);
	return ext;
}

// fileHasExtension (single string)
//
// Thin wrapper -- promote the single extension to a one-element
// vector and forward to the vector overload below for the actual
// case-insensitive match.
bool fileHasExtension(
    const std::string& path,
    const std::string& extension
) {
	std::vector<std::string> extensions;
	extensions.push_back(extension);
	return fileHasExtension(path, extensions);
}

// fileHasExtension (NULL-terminated C array)
//
// Builds a vector by walking until the NULL sentinel, then defers
// to the vector overload. Callers MUST include the NULL terminator
// -- without it the loop reads past the end.
bool fileHasExtension(
    const std::string& path,
    const char** extensions
) {
	if (!extensions)
		return false;

	std::vector<std::string> extList;
	uint32_t extCount = 0;

	while (true) {
		if (!extensions[extCount])
			break;

		extList.push_back(extensions[extCount]);
		extCount++;
	}

	return fileHasExtension(path, extList);
}

// fileHasExtension (vector)
//
// The real implementation. Case-insensitive comparison via strcasecmp,
// skipping any empty strings in the candidate list. Returns false if
// the path has no extension, or if the candidate list is empty.
bool fileHasExtension(
    const std::string& path,
    const std::vector<std::string>& extensions
) {
	const std::string pathExtension = fileExtension(path);
	const size_t numExtensions = extensions.size();

	if (pathExtension.size() == 0)
		return false;

	if (numExtensions == 0)
		return false;

	for (size_t n = 0; n < numExtensions; n++) {
		if (extensions[n].size() == 0)
			continue;

		if (strcasecmp(pathExtension.c_str(), extensions[n].c_str()) == 0)
			return true;
	}

	return false;
}

// fileRemoveExtension
//
// Guarded against the "dot in directory segment" case: if the last
// `.` precedes the last `/`, the path has no real extension and is
// returned unchanged. So fileRemoveExtension("foo.bar/baz") -> "foo.bar/baz",
// not "foo.bar/baz" stripped to "foo".
std::string fileRemoveExtension(
    const std::string& filename
) {
	const std::string::size_type dotIdx = filename.find_last_of(".");
	const std::string::size_type slashIdx = filename.find_last_of("/");

	if (dotIdx == std::string::npos)
		return filename;

	if (slashIdx != std::string::npos && dotIdx < slashIdx)
		return filename;

	return filename.substr(0, dotIdx);
}

// fileChangeExtension
//
// NOTE: concatenation only -- the `.` is NOT inserted. Caller must
// include the leading dot in `newExtension`:
//   fileChangeExtension("a.xml", ".zip")  -> "a.zip"
//   fileChangeExtension("a.xml",  "zip")  -> "azip"
// Surprising but documented behaviour; matching upstream.
std::string fileChangeExtension(
    const std::string& filename,
    const std::string& newExtension
) {
	return fileRemoveExtension(filename).append(newExtension);
}
