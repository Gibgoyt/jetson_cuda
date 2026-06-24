/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
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
 *	logging.cpp  --  implementation of the static Log class
 *	=======================================================
 *
 *	See logging.h for the public API and the macro/safety gotchas.
 *	This file is just the static-member initializers and four small
 *	configuration entry points (ParseCmdLine, SetFile x2, LevelToStr /
 *	LevelFromStr).
 *
 *	The log-call hot path (the LogError/LogInfo/... macros) lives
 *	entirely in the header -- they expand to a single `if (level <=
 *	Log::GetLevel()) fprintf(...)` at the call site. Nothing in this
 *	.cpp is on that path. This file only runs during configuration
 *	(typically once per process, at commandLine ctor time).
*/

#include "logging.h"
#include <strings.h>


//-----------------------------------------------------------------------------------
// Static state -- exactly one logger per process
//-----------------------------------------------------------------------------------
// Defaults: VERBOSE threshold (so most messages get through), stdout sink,
// "stdout" as the human-readable name. The header's Log::DEFAULT alias is
// `VERBOSE`; the comment "set default logging options" from upstream is
// what this stanza is.

// set default logging options
Log::Level Log::mLevel = Log::DEFAULT;
FILE* Log::mFile = stdout;
std::string Log::mFilename = "stdout";


//-----------------------------------------------------------------------------------
// ParseCmdLine -- read --log-level / --log-file / --verbose / --debug
//-----------------------------------------------------------------------------------

// ParseCmdLine
//
// Convenience overload: build a temporary commandLine and forward. Note
// that commandLine's constructor itself calls Log::ParseCmdLine(*this),
// so going through this entry point can recurse one level deep. Harmless,
// but it's the reason direct callers usually use the other overload.
void Log::ParseCmdLine(
    const int argc,
    char** argv
) {
	ParseCmdLine(commandLine(argc, argv));
}

// ParseCmdLine
//
// Order matters here. --log-level wins outright if present; otherwise
// --verbose / --debug fire (and DEBUG wins over VERBOSE because it's
// tested last). Then --log-file is applied, after which the level
// override might no longer apply if the file was changed -- but in
// practice it always does, since SetLevel writes mLevel directly.
//
// NOTE: the `setbuf(stdout, NULL)` call at the bottom is a GLOBAL side
//       effect on the program's stdout, not just the log subsystem.
//       The rationale (per the comment) is making post-newline ANSI
//       colour resets paint promptly. If you want buffered stdout for
//       other purposes, route the log via `--log-file=/dev/stdout` so
//       this branch doesn't fire (or via /dev/stderr).
void Log::ParseCmdLine(
    const commandLine& cmdLine
) {
	const char* levelStr = cmdLine.GetString("log-level");

	if (levelStr != NULL) {
		// Note: LevelFromStr falls back to DEFAULT (== VERBOSE) on
		// unknown input, so a typo'd --log-level doesn't error out --
		// it silently keeps the default.
		SetLevel(LevelFromStr(levelStr));
	} else {
		// Shortcuts: --verbose first, --debug second. Both can fire
		// in the same invocation -- DEBUG wins because it's last.
		if (cmdLine.GetFlag("verbose"))
			SetLevel(VERBOSE);

		if (cmdLine.GetFlag("debug"))
			SetLevel(DEBUG);
	}

	// SetFile(NULL) is a no-op, so missing --log-file leaves the default
	// stdout sink in place.
	SetFile(cmdLine.GetString("log-file"));

	// disable buffering so that the post-newline color resets are used
	// (https://stackoverflow.com/a/1716621)
	//
	// NOTE: this is a GLOBAL side effect on the program's stdout, not
	// just the logger. Only fires when the sink stayed (or became) stdout.
	if (mFile == stdout)
		setbuf(stdout, NULL);
}


//-----------------------------------------------------------------------------------
// SetFile (FILE*) and SetFile (const char*)
//-----------------------------------------------------------------------------------
// Two overloads -- raw FILE* and string. The string form does the
// stdout/stderr alias-check, then falls back to fopen("w") for a real
// path. Both ultimately funnel through the FILE* overload to update mFile.

// SetFile
//
// NULL rejected; same-file is a no-op. Does NOT fclose any previously
// installed custom FILE -- caller owns custom FILE lifetimes. Only
// updates mFilename when the sink is stdout/stderr; switching to a
// user-opened FILE leaves the old filename string in place, so
// GetFilename can be stale after this path.
void Log::SetFile(
    FILE* file
) {
	if (!file || mFile == file)
		return;

	mFile = file;

	if (mFile == stdout)
		mFilename = "stdout";
	else if (mFile == stderr)
		mFilename = "stderr";
}

// SetFilename
//
// NOTE: the upstream banner comment says "SetFilename" but the function
// is actually SetFile(const char*). Likely leftover from a rename.
//
// String dispatch:
//   "stdout" / "stderr" (case-insensitive) -> alias to FILE*
//   anything else                          -> fopen(filename, "w")
//
// The "w" mode TRUNCATES the target on every successful open. Repeated
// calls with the same filename short-circuit via the strcasecmp early
// return, so single-filename usage is idempotent. Two distinct filenames
// in the same run will each be truncated when they're selected.
void Log::SetFile(
    const char* filename
) {
	if (!filename)
		return;

	if (strcasecmp(filename, "stdout") == 0)
		SetFile(stdout);
	else if (strcasecmp(filename, "stderr") == 0)
		SetFile(stderr);
	else {
		// Same filename twice in a row -> no-op, so callers don't
		// accidentally truncate an active log on reconfigure.
		if (strcasecmp(filename, mFilename.c_str()) == 0)
			return;

		FILE* file = fopen(filename, "w");

		if (file != NULL) {
			SetFile(file);
			// Manually update mFilename here because SetFile(FILE*)
			// only does so for stdout/stderr.
			mFilename = filename;
		} else {
			LogError("failed to open '%s' for logging\n", filename);
			return;
		}
	}
}


//-----------------------------------------------------------------------------------
// LevelToStr / LevelFromStr -- enum <-> human-readable name
//-----------------------------------------------------------------------------------

// LevelToStr
//
// Trivial enum-to-string switch. The implicit DEFAULT case (returning
// "default") is unreachable for any value produced by LevelFromStr,
// because DEFAULT == VERBOSE numerically and the VERBOSE case fires
// first. So in practice the function always returns one of the seven
// canonical names.
const char* Log::LevelToStr(
    Log::Level level
) {
	switch (level) {
	case SILENT:
		return "silent";
	case ERROR:
		return "error";
	case WARNING:
		return "warning";
	case SUCCESS:
		return "success";
	case INFO:
		return "info";
	case VERBOSE:
		return "verbose";
	case DEBUG:
		return "debug";
	}

	return "default";
}

// LevelFromStr
//
// Two-phase parse:
//   1. Loop n=0..DEBUG, compare against LevelToStr(n). First case-
//      insensitive match wins. This handles silent/error/warning/
//      success/info/verbose/debug.
//   2. Aliases for SILENT: "disable" / "disabled" / "none".
//
// NOTE: anything else returns DEFAULT (== VERBOSE) silently. There is no
//       way for a caller to distinguish "user passed garbage" from "user
//       asked for the default level". --log-level=blarg therefore looks
//       identical to --log-level=verbose.
Log::Level Log::LevelFromStr(
    const char* str
) {
	if (!str)
		return DEFAULT;

	for (int n = 0; n <= DEBUG; n++) {
		const Level level = (Level)n;

		if (strcasecmp(str, LevelToStr(level)) == 0)
			return level;
	}

	// Undocumented aliases for SILENT (not listed in LOG_USAGE_STRING).
	if (strcasecmp(str, "disable") == 0 || strcasecmp(str, "disabled") == 0 ||
	    strcasecmp(str, "none") == 0)
		return SILENT;

	return DEFAULT;
}
