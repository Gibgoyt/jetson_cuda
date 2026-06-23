/*
 * Copyright (c) 2017, NVIDIA CORPORATION. All rights reserved.
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

// =============================================================================
//  commandLine.cpp  --  implementation of the argv parser
// -----------------------------------------------------------------------------
//  How the named-argument getters work (the shared scan pattern):
//    1. For each entry argv[i], count the leading `-` characters via
//       strFindDelimiter. If there are none (or the entry is all dashes),
//       skip it -- it isn't a named flag.
//    2. Take the substring AFTER the dashes and case-insensitively prefix-
//       compare it against the requested argName using `strncasecmp`. A
//       prefix match is enough for GetInt/GetFloat/GetString because the
//       value portion (after `=`) is parsed separately. GetFlag is the
//       exception: it requires an exact match up to either end-of-string
//       or `=`.
//    3. On hit, parse the value:
//         GetInt   -> atoi  starting at the byte after `=` (0 if no `=`)
//         GetFloat -> atof  starting at the byte after `=` (0.f if no `=`)
//         GetString-> pointer to the byte after `=` (aliases into argv)
//         GetFlag  -> just `true`
//    4. On miss, if `allowOtherDelimiters` is true, retry once with `-` and
//       `_` swapped throughout argName. The retry sets allowOtherDelimiters
//       to false so it cannot recurse further.
//
//  Index asymmetry (gotcha):
//    The macro ARGC_START is `0`, and the named-arg getters scan from i=0,
//    so they technically scan argv[0] (the program path) too. That happens
//    to be safe because a program path like "./detectnet" has no leading
//    `-` and strFindDelimiter returns 0 for it, so it is skipped.
//    The positional getters, however, use `i = 1 /*ARGC_START*/` -- a stale
//    comment; they explicitly skip argv[0]. Keep this asymmetry in mind if
//    you ever refactor.
//
//  Constructor side-effect:
//    Both ctors end with Log::ParseCmdLine(*this), so constructing a
//    commandLine is also what configures the global logger from flags like
//    `--log-level=verbose`.
// =============================================================================

#include "commandLine.h"
#include "logging.h"

#include <string>
#include <string.h>
#include <strings.h>

// ARGC_START is the loop start index for the named-arg getters below.
// It is `0` -- meaning argv[0] (the program path) is scanned but harmless,
// since program paths don't start with `-`. Note GetPosition/GetPositionArgs
// override this with `i = 1`; the `/*ARGC_START*/` comment there is stale.
#define ARGC_START 0


// =============================================================================
// --- Static helpers ----------------------------------------------------------
// =============================================================================

// search for the end of a leading character in a string (e.g. '--foo')
//
// Counts the run of leading `delimiter` characters and returns the index of
// the first non-delimiter byte -- i.e. for "--foo" with delimiter '-' it
// returns 2. Returns 0 (treated as "not a flag" by callers) when the string
// is empty, contains no leading delimiter, or is *entirely* made of
// delimiters (e.g. a bare "--").
static inline int strFindDelimiter(char delimiter, const char* string) {
	int string_start = 0;

	while (string[string_start] == delimiter)
		string_start++;

	if (string_start >= (int)strlen(string) - 1)
		return 0;

	return string_start;
}

// replace hyphens for underscores and vice-versa (returns NULL if no changes)
//
// Used by every named-arg getter to retry once with the opposite delimiter
// style when the original lookup fails (so `--foo-bar` and `--foo_bar` are
// interchangeable). Caller OWNS the returned buffer and must `free()` it.
//
// NOTE: off-by-one bug -- malloc(str_length) does not include space for the
//       null terminator, but strcpy writes str_length+1 bytes. This is a
//       latent heap-corruption bug that only fires on the `_<->-` retry
//       path (i.e. when a flag exists in argName but doesn't match argv on
//       the first pass). Worth fixing to `malloc(str_length + 1)`.
static inline char* strSwapDelimiter(const char* string) {
	if (!string)
		return NULL;

	// determine if the original char is in the string
	bool found = false;
	const int str_length = strlen(string);

	for (int n = 0; n < str_length; n++) {
		if (string[n] == '-' || string[n] == '_') {
			found = true;
			break;
		}
	}

	if (!found)
		return NULL;

	// allocate a new string to modify
	char* new_str = (char*)malloc(str_length);  // NOTE: should be str_length + 1

	if (!new_str)
		return NULL;

	strcpy(new_str, string);

	// replace instances of the old char
	for (int n = 0; n < str_length; n++) {
		if (new_str[n] == '-')
			new_str[n] = '_';
		else if (new_str[n] == '_')
			new_str[n] = '-';
	}

	return new_str;
}


// =============================================================================
// --- Constructors (also initializes logging) ---------------------------------
// =============================================================================
// Both forms just stash argc/argv (no deep copy -- argv must outlive `this`),
// optionally inject an extra flag/arg-list, then hand the result to
// Log::ParseCmdLine so flags like --log-level / --verbose take effect
// before the rest of main() runs.

// constructor
commandLine::commandLine(const int pArgc, char** pArgv, const char* extraFlag) {
	argc = pArgc;
	argv = pArgv;

	AddFlag(extraFlag);

	Log::ParseCmdLine(*this);
}

// constructor
commandLine::commandLine(const int pArgc, char** pArgv, const char** extraArgs) {
	argc = pArgc;
	argv = pArgv;

	AddArgs(extraArgs);

	Log::ParseCmdLine(*this);
}


// =============================================================================
// --- Named-argument getters (shared scan pattern) ----------------------------
// =============================================================================
// See the file-header block for the canonical description of the scan
// pattern. The four getters below all follow the same shape:
//   - early-out if argc < 1
//   - loop argv looking for a leading `-` + case-insensitive prefix match
//   - on hit, parse the value (after `=`) according to the target type
//   - on miss, recurse once with delimiters swapped (`-` <-> `_`)
// Subtlety: the loop continues after a hit (doesn't `break`) so the LAST
// occurrence on the command line wins -- this lets later `--foo=...` flags
// override earlier ones, which is how AddFlag-style overrides work.

// GetInt
int commandLine::GetInt(
    const char* string_ref,
    int default_value,
    bool allowOtherDelimiters
) const {
	if (argc < 1)
		return default_value;

	bool bFound = false;
	int value = -1;

	for (int i = ARGC_START; i < argc; i++) {
		const int string_start = strFindDelimiter('-', argv[i]);

		if (string_start == 0)
			continue;

		const char* string_argv = &argv[i][string_start];
		const int length = (int)strlen(string_ref);

		if (!strncasecmp(string_argv, string_ref, length)) {
			// value follows the `=` if present, otherwise treat as 0
			if (length + 1 <= (int)strlen(string_argv)) {
				int auto_inc = (string_argv[length] == '=') ? 1 : 0;
				value = atoi(&string_argv[length + auto_inc]);
			} else {
				value = 0;
			}

			bFound = true;
			continue;  // keep looping so the last occurrence wins
		}
	}

	if (bFound)
		return value;

	if (!allowOtherDelimiters)
		return default_value;

	// retry once with `-` <-> `_` swapped (e.g. `foo-bar` <-> `foo_bar`)
	char* swapped_ref = strSwapDelimiter(string_ref);

	if (!swapped_ref)
		return default_value;

	value = GetInt(swapped_ref, default_value, false);
	free(swapped_ref);
	return value;
}

// GetUnsignedInt
//
// Thin wrapper over GetInt that clamps negative parse results (e.g. a user
// passing `--foo=-5`) back to defaultValue.
uint32_t commandLine::GetUnsignedInt(
    const char* argName,
    uint32_t defaultValue,
    bool allowOtherDelimiters
) const {
	const int val = GetInt(argName, (int)defaultValue, allowOtherDelimiters);

	if (val < 0)
		return defaultValue;

	return val;
}

// GetFloat
//
// Identical control flow to GetInt; only the value-parse step differs (atof
// in place of atoi).
float commandLine::GetFloat(
    const char* string_ref,
    float default_value,
    bool allowOtherDelimiters
) const {
	if (argc < 1)
		return default_value;

	bool bFound = false;
	float value = -1;

	for (int i = ARGC_START; i < argc; i++) {
		const int string_start = strFindDelimiter('-', argv[i]);

		if (string_start == 0)
			continue;

		const char* string_argv = &argv[i][string_start];
		const int length = (int)strlen(string_ref);

		if (!strncasecmp(string_argv, string_ref, length)) {
			if (length + 1 <= (int)strlen(string_argv)) {
				int auto_inc = (string_argv[length] == '=') ? 1 : 0;
				value = (float)atof(&string_argv[length + auto_inc]);
			} else {
				value = 0.f;
			}

			bFound = true;
			continue;  // last occurrence wins
		}
	}

	if (bFound)
		return value;

	if (!allowOtherDelimiters)
		return default_value;

	// retry once with `-` <-> `_` swapped
	char* swapped_ref = strSwapDelimiter(string_ref);

	if (!swapped_ref)
		return default_value;

	value = GetFloat(swapped_ref, default_value, false);
	free(swapped_ref);
	return value;
}

// GetFlag
//
// Unlike the typed getters above, GetFlag uses an *exact* token match: the
// argv substring after the dashes must equal string_ref up to either its end
// or an `=`. This is what prevents `--foobar` from satisfying GetFlag("foo")
// (which would be wrong) while still letting GetString("foo") match
// `--foo=...` via prefix.
bool commandLine::GetFlag(const char* string_ref, bool allowOtherDelimiters) const {
	if (argc < 1)
		return false;

	for (int i = ARGC_START; i < argc; i++) {
		const int string_start = strFindDelimiter('-', argv[i]);

		if (string_start == 0)
			continue;

		const char* string_argv = &argv[i][string_start];
		const char* equal_pos = strchr(string_argv, '=');

		// argv token length stops at the `=` if one exists (so we compare
		// just the flag name, not the value)
		const int argv_length =
		    (int)(equal_pos == 0 ? strlen(string_argv) : equal_pos - string_argv);
		const int length = (int)strlen(string_ref);

		if (length == argv_length && !strncasecmp(string_argv, string_ref, length))
			return true;
	}

	if (!allowOtherDelimiters)
		return false;

	// retry once with `-` <-> `_` swapped
	char* swapped_ref = strSwapDelimiter(string_ref);

	if (!swapped_ref)
		return false;

	const bool value = GetFlag(swapped_ref, false);
	free(swapped_ref);
	return value;
}

// GetString
//
// Same scan pattern as GetInt/GetFloat, but returns a pointer that aliases
// into argv -- caller does NOT own it and must not free it.
//
// NOTE: this returns `string_argv + length + 1` *unconditionally* on a
//       prefix match. For `--foo=bar` that points at "bar" (correct). But
//       for a bare `--foo` with no value, length+1 lands one byte past the
//       null terminator -- undefined behavior. GetInt/GetFloat guard against
//       this with their `length + 1 <= strlen(...)` check; GetString does
//       not. Also note GetString uses prefix match, so `GetString("foo")`
//       will incorrectly fire on `--foobar=baz` -- in practice this is rare
//       because callers know their own arg names.
const char* commandLine::GetString(
    const char* string_ref,
    const char* default_value,
    bool allowOtherDelimiters
) const {
	if (argc < 1)
		return default_value;

	for (int i = ARGC_START; i < argc; i++) {
		const int string_start = strFindDelimiter('-', argv[i]);

		if (string_start == 0)
			continue;

		char* string_argv = (char*)&argv[i][string_start];
		const int length = (int)strlen(string_ref);

		if (!strncasecmp(string_argv, string_ref, length))
			return (string_argv + length + 1);  // points just past the `=`
		//*string_retval = &string_argv[length+1];
	}

	if (!allowOtherDelimiters)
		return default_value;

	// retry once with `-` <-> `_` swapped
	char* swapped_ref = strSwapDelimiter(string_ref);

	if (!swapped_ref)
		return default_value;

	const char* value = GetString(swapped_ref, default_value, false);
	free(swapped_ref);
	return value;
}


// =============================================================================
// --- Positional getters (skip argv[0]) ---------------------------------------
// =============================================================================
// "Positional" = an argv entry whose strFindDelimiter('-', ...) returns 0,
// i.e. it doesn't start with a dash. Note these explicitly start the loop
// at i=1 (the `/*ARGC_START*/` comment is misleading -- ARGC_START is 0,
// the loop simply hardcodes 1 here to skip the program name).

// GetPosition
const char* commandLine::GetPosition(unsigned int position, const char* default_value) const {
	if (argc < 1 || position >= GetPositionArgs())
		return default_value;

	unsigned int position_count = 0;

	for (int i = 1 /*ARGC_START*/; i < argc; i++) {
		const int string_start = strFindDelimiter('-', argv[i]);

		if (string_start != 0)
			continue;

		if (position == position_count)
			return argv[i];

		position_count++;
	}

	return default_value;
}

// GetPositionArgs
unsigned int commandLine::GetPositionArgs() const {
	unsigned int position_count = 0;

	for (int i = 1 /*ARGC_START*/; i < argc; i++) {
		const int string_start = strFindDelimiter('-', argv[i]);

		if (string_start != 0)
			continue;

		position_count++;
	}

	return position_count;
}


// =============================================================================
// --- Mutators (AddArg / AddArgs / AddFlag) -----------------------------------
// =============================================================================
// Each AddArg call grows argv by one slot: allocate a new pointer array of
// size argc+1, copy existing pointers, allocate+strcpy the new string into
// the last slot, then swap argv. The original argv (from main()) is *not*
// owned by this object, so it deliberately isn't freed; subsequent grows
// leak the intermediate pointer arrays (rarely a concern since AddArg is
// only called a few times during construction).
//
// NOTE: the per-call malloc of the new argv pointer array is leaked on every
//       call after the first. Acceptable in practice given the call volume,
//       but worth knowing if this class ever grows a destructor.

// AddArg
void commandLine::AddArg(const char* arg) {
	if (!arg)
		return;

	const size_t arg_length = strlen(arg);

	if (arg_length == 0)
		return;

	const int new_argc = argc + 1;
	char** new_argv = (char**)malloc(sizeof(char*) * new_argc);

	if (!new_argv)
		return;

	for (int n = 0; n < argc; n++)
		new_argv[n] = argv[n];

	new_argv[argc] = (char*)malloc(arg_length + 1);

	if (!new_argv[argc])
		return;

	strcpy(new_argv[argc], arg);

	argc = new_argc;
	argv = new_argv;
}

// AddArgs
//
// Walks a NULL-terminated `char**` and forwards each entry to AddArg.
void commandLine::AddArgs(const char** args) {
	if (!args)
		return;

	int arg_count = 0;

	while (true) {
		if (!args[arg_count])
			return;

		AddArg(args[arg_count]);
		arg_count++;
	}
}

// AddFlag
//
// Convenience: prepends `--` and dedupes via GetFlag so the same flag isn't
// added twice. This is the path the `extraFlag` ctor parameter takes -- it's
// how callers like `IS_HEADLESS()` inject `--headless` automatically.
void commandLine::AddFlag(const char* flag) {
	if (!flag || strlen(flag) == 0)
		return;

	if (GetFlag(flag))
		return;

	const std::string arg = std::string("--") + flag;
	AddArg(arg.c_str());
}


// =============================================================================
// --- Print -------------------------------------------------------------------
// =============================================================================

// Print
//
// Dumps the current argv, space-separated, followed by a newline. Used for
// debugging / `--help`-style echoes; not part of the parsing API.
void commandLine::Print() const {
	for (int n = 0; n < argc; n++)
		printf("%s ", argv[n]);

	printf("\n");
}
