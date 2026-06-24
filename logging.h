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
 *	logging.h  --  the project-wide log printer (Log class + Log{Error,Info,...} macros)
 *	====================================================================================
 *
 *	Position in the pipeline (Stage 1, "Pure C++ foundations" per PLAN.md
 *	-- fourth file in the reading order, after commandLine + filesystem):
 *		This is the LogError / LogInfo / LogVerbose / ... family that
 *		nearly every other file in the project funnels its output
 *		through. CUDA errors, TensorRT load progress, GStreamer pipeline
 *		state -- all of it eventually expands to a printf call here,
 *		gated on a single global threshold.
 *
 *		commandLine's constructor invokes Log::ParseCmdLine, so just
 *		constructing the argv parser also configures this. By the time
 *		main()'s first non-cmdLine line runs, the logger has already
 *		picked up --log-level / --log-file / --verbose / --debug.
 *
 *	What this file provides:
 *
 *		Static class
 *			Log                    -- holds the threshold (mLevel), the
 *			                          FILE* sink (mFile), and the
 *			                          filename string used to identify
 *			                          the sink at runtime. All members
 *			                          are static; do NOT instantiate.
 *
 *		Log::Level enum (severity, ordered SILENT < ... < DEBUG)
 *			SILENT  ERROR  WARNING  SUCCESS  INFO  VERBOSE  DEBUG
 *			DEFAULT == VERBOSE.
 *			A message is printed iff `messageLevel <= mLevel`.
 *
 *		Six call-site macros (one per non-SILENT level)
 *			LogError(fmt, ...)      red,    LOG_LEVEL_PREFIX_ERROR
 *			LogWarning(fmt, ...)    yellow, LOG_LEVEL_PREFIX_WARNING
 *			LogSuccess(fmt, ...)    green,  LOG_LEVEL_PREFIX_SUCCESS
 *			LogInfo(fmt, ...)       (no colour), LOG_LEVEL_PREFIX_INFO
 *			LogVerbose(fmt, ...)    (no colour), LOG_LEVEL_PREFIX_VERBOSE
 *			LogDebug(fmt, ...)      (no colour), LOG_LEVEL_PREFIX_DEBUG
 *
 *		One internal macro
 *			GenericLogMessage(level, fmt, ...) -- the if-then-fprintf
 *			body the wrappers expand to. @internal -- don't call.
 *
 *		Compile-time switches
 *			-DLOG_DISABLE_COLORS         strip ANSI escape sequences
 *			-DLOG_ENABLE_LEVEL_PREFIX    enable [E]/[W]/[S]/... prefixes
 *
 *		Command-line integration (consumed by Log::ParseCmdLine)
 *			--log-level=<level>      one of silent/error/warning/success/
 *			                          info/verbose/debug
 *			                          (also accepts disable/disabled/none
 *			                          as aliases for silent -- undocumented
 *			                          in Usage())
 *			--log-file=<path>        any path, or the literal strings
 *			                          "stdout" / "stderr"
 *			--verbose                shorthand for --log-level=verbose
 *			--debug                  shorthand for --log-level=debug
 *
 *	Behaviour worth remembering when reading code that uses these:
 *
 *		1. The macros are NOT statement-safe.
 *		   GenericLogMessage expands to a bare `if (...) fprintf(...);`
 *		   with no braces and no `do { ... } while (0)` wrap. So:
 *		     if (x) LogError("a"); else foo();
 *		   silently makes `else` bind to the macro's hidden `if`. The
 *		   wrappers (LogError, LogWarning, ...) inherit this footgun.
 *		   Use braces at every conditional log call site.
 *
 *		2. ALL severities go to one FILE.
 *		   There is no stderr/stdout split based on level -- LogError
 *		   goes to whatever Log::mFile points at (default stdout). Shell
 *		   users `prog | grep` will catch errors in the pipe; redirect
 *		   the file explicitly with --log-file=/dev/stderr if you want
 *		   the conventional separation.
 *
 *		3. ANSI colours are baked into the format string.
 *		   Each colour-bearing macro wraps `format` with LOG_COLOR_RED /
 *		   LOG_COLOR_RESET (etc.) BEFORE fprintf, with no terminal-vs-file
 *		   detection. A `--log-file=foo.log` will get raw `\033[0;31m`
 *		   escapes in it unless you also build with -DLOG_DISABLE_COLORS.
 *
 *		4. Variadic macros use the GCC `args...` extension.
 *		   Not portable to MSVC -- they'd need `__VA_ARGS__` and
 *		   `##__VA_ARGS__`. Fine on Jetson L4T (gcc/clang).
 *
 *		5. Level prefixes are off by default.
 *		   The `[E]` / `[W]` / `[S]` / ... prefixes only appear when
 *		   -DLOG_ENABLE_LEVEL_PREFIX is set. Greppable severity needs
 *		   either that flag or the colour codes themselves.
 *
 *		6. Unknown --log-level silently falls back to VERBOSE.
 *		   See LevelFromStr in the .cpp -- a typo'd flag value never
 *		   surfaces an error, it just keeps the default. The undocumented
 *		   "disable" / "disabled" / "none" aliases do work for SILENT.
 *
 *		7. ParseCmdLine has a global side-effect.
 *		   It calls `setbuf(stdout, NULL)` when the sink is stdout, so
 *		   the WHOLE program's stdout becomes unbuffered (not just the
 *		   logger). The motivation is making post-newline ANSI colour
 *		   resets paint promptly (see Stack Overflow link in source).
 *
 *	Cross-references:
 *		commandLine.h -- ParseCmdLine reads flags from one of these.
 *		                  commandLine's ctor calls back into Log, so
 *		                  this header has a quasi-circular relationship
 *		                  with commandLine.h. Resolved by including only
 *		                  the lightweight commandLine declaration here.
*/

#if !defined(__LOGGING_UTILS_H_)
	#define __LOGGING_UTILS_H_

	#include "commandLine.h"

	#include <stdio.h>
	#include <string>

	/**
	 * Standard command-line options able to be passed to videoOutput::Create()
	 * @ingroup log
	 *
	 *	NOTE: the doxygen line above is wrong in upstream -- this string
	 *	is the Log usage, not videoOutput's. Kept verbatim so generated
	 *	docs match upstream. Also note "disable" / "disabled" / "none"
	 *	are accepted as aliases for `silent` but NOT listed here.
	*/
	#define LOG_USAGE_STRING                                                                           \
		"logging arguments: \n"                                                                        \
		"  --log-file=FILE        output destination file (default is stdout)\n"                       \
		"  --log-level=LEVEL      message output threshold, one of the following:\n"                   \
		"                             * silent\n"                                                      \
		"                             * error\n"                                                       \
		"                             * warning\n"                                                     \
		"                             * success\n"                                                     \
		"                             * info\n"                                                        \
		"                             * verbose (default)\n"                                           \
		"                             * debug\n"                                                       \
		"  --verbose              enable verbose logging (same as --log-level=verbose)\n"              \
		"  --debug                enable debug logging   (same as --log-level=debug)\n\n"

	/**
	 * Message logging with a variable level of output and destinations.
	 * @ingroup log
	 *
	 *	Static-only class -- all state lives in the three static
	 *	members at the bottom (mLevel, mFile, mFilename) and all methods
	 *	are static. Don't construct one. There is exactly one logger
	 *	per process.
	*/
	class Log {
	public:
		/**
		 * Defines the logging level of a message, and the threshold
		 * used by the logger to either drop or output messages.
		 *
		 *	Numerically ordered: SILENT(0) < ERROR < ... < DEBUG. A
		 *	message at level `m` is emitted iff `m <= mLevel`. So
		 *	setting mLevel = ERROR drops everything below ERROR severity,
		 *	while mLevel = DEBUG emits everything.
		*/
		enum Level {
			SILENT = 0, /**< No messages are output. */
			ERROR,      /**< Major errors that may impact application execution. */
			WARNING,    /**< Warning conditions where the application may be able to proceed in some
			               capacity. */
			SUCCESS,    /**< Successful events (e.g. the loading or creation of a resource) */
			INFO,       /**< Informational messages that are more important than VERBOSE messages */
			VERBOSE,    /**< Verbose details about program execution */
			DEBUG,      /**< Low-level debugging (disabled by default) */
			DEFAULT = VERBOSE /**< The default level is `VERBOSE` */
		};

		/**
		 * Get the current logging level.
		*/
		static inline Level GetLevel() {
			return mLevel;
		}

		/**
		 * Set the current logging level.
		*/
		static inline void SetLevel(
		    Level level
		) {
			mLevel = level;
		}

		/**
		 * Get the current log output.
		 *
		 *	The raw FILE* the logger writes to. Defaults to stdout.
		 *	Never NULL after construction -- SetFile rejects NULL.
		*/
		static inline FILE* GetFile() {
			return mFile;
		}

		/**
		 * Get the filename of the log output.
		 * This may also return `"stdout"` or `"stderror"`.
		 *
		 *	NOTE: the docstring above is wrong in upstream -- the actual
		 *	      string returned for stderr is `"stderr"`, not
		 *	      `"stderror"`. Kept verbatim to match upstream docs.
		 *	NOTE: if the user calls `SetFile(FILE*)` with a custom FILE
		 *	      (i.e. not stdout/stderr), mFilename is NOT updated --
		 *	      this getter will return a stale name from a previous
		 *	      SetFile(const char*) call (or the default "stdout").
		*/
		static inline const char* GetFilename() {
			return mFilename.c_str();
		}

		/**
		 * Set the logging output.
		 * This can be a built-in file, like `stdout` or `stderr`,
		 * or a file that has been opened by the user.
		 *
		 *	NULL is rejected (no-op). Passing the current sink is also a
		 *	no-op. Does NOT fclose the previous sink -- if the caller
		 *	opened a custom FILE and is now switching away from it, they
		 *	must close the old one themselves.
		 *
		 *	mFilename is only updated when `file` is stdout/stderr;
		 *	custom FILEs leave the old filename string in place. Use the
		 *	`SetFile(const char*)` overload if you want GetFilename to
		 *	report something meaningful.
		*/
		static void SetFile(
		    FILE* file
		);

		/**
		 * Set the logging output.
		 * Can be `"stdout"`, `"stderr"`, `"log.txt"`, ect.
		 *
		 *	"stdout" and "stderr" (case-insensitive) are aliased to the
		 *	corresponding FILE*. Any other string is treated as a file
		 *	path and opened with mode `"w"` (truncating). A LogError is
		 *	emitted on open failure and the previous sink is preserved.
		 *
		 *	NOTE: passing the same filename twice in a row is a no-op
		 *	(early return via strcasecmp against mFilename). Passing two
		 *	different filenames within a single run truncates each in
		 *	turn -- this is not a log-rotation API.
		*/
		static void SetFile(
		    const char* filename
		);

		/**
		 * Usage string for command line arguments to Create()
		*/
		static inline const char* Usage() {
			return LOG_USAGE_STRING;
		}

		/**
		 * Parse command line options (see Usage() above)
		 *
		 *	Convenience overload -- wraps `argc`/`argv` in a temporary
		 *	commandLine and forwards to the other ParseCmdLine. If you
		 *	already have a commandLine, prefer the other overload to
		 *	avoid re-parsing.
		*/
		static void ParseCmdLine(
		    const int argc,
		    char** argv
		);

		/**
		 * Parse command line options (see Usage() above)
		 *
		 *	Order of effect:
		 *	  1. Read --log-level (if present, wins).
		 *	  2. Else, --verbose / --debug shortcuts (debug wins over
		 *	     verbose because it's tested last).
		 *	  3. Read --log-file (NULL means "leave default").
		 *	  4. If the resulting sink is stdout, `setbuf(stdout, NULL)`
		 *	     -- GLOBAL side effect on the program's stdout.
		 *
		 *	Called automatically from commandLine's ctor, so most code
		 *	never invokes this directly.
		*/
		static void ParseCmdLine(
		    const commandLine& cmdLine
		);

		/**
		 * Convert a logging level to string.
		*/
		static const char* LevelToStr(
		    Level level
		);

		/**
		 * Parse a logging level from a string.
		 *
		 *	NOTE: silently returns DEFAULT (== VERBOSE) for unknown
		 *	input -- including empty strings, typos, or NULL. There is
		 *	no error indication. Also accepts "disable" / "disabled" /
		 *	"none" as undocumented aliases for SILENT.
		*/
		static Level LevelFromStr(
		    const char* str
		);

	protected:
		// Static state -- exactly one logger per process. Defaults
		// initialized in logging.cpp: SILENT-incl-VERBOSE level, stdout
		// sink, "stdout" name.
		static Level mLevel;
		static FILE* mFile;
		static std::string mFilename;
	};

	/*
	 *	------------------------------------------------------------------
	 *	Call-site logging macros
	 *	------------------------------------------------------------------
	 *	All six wrappers go through GenericLogMessage. They share the
	 *	same statement-safety footgun: the macro expands to a bare
	 *	`if (level <= ...) fprintf(...);` with NO surrounding braces
	 *	and NO `do { ... } while (0)` wrap. Consequences:
	 *	  - At call sites inside an unbraced `if` / `else`, the caller's
	 *	    `else` can bind to the macro's hidden `if`.
	 *	  - The macro is NOT a single statement -- chained side-effects
	 *	    after it (separated by `,` or in a comma operator) won't
	 *	    behave as expected.
	 *	Always use braces around conditional log call sites.
	 *
	 *	Variadic syntax (`args...` rather than `__VA_ARGS__`) is the GCC
	 *	extension -- portable to clang/gcc, not to MSVC.
	*/

	/**
	 * Log a printf-style message with the provided level.
	 * @ingroup log
	 * @internal
	 *
	 *	Don't call this directly -- use one of the six level-specific
	 *	wrappers below. The wrappers add the colour escapes and level
	 *	prefix to `format`; this macro is just the threshold check + fprintf.
	*/
	#define GenericLogMessage(level, format, args...)                                                  \
		if (level <= Log::GetLevel())                                                                  \
		fprintf(Log::GetFile(), format, ##args)

	/**
	 * Log a printf-style error message (Log::ERROR)
	 * @ingroup log
	 *
	 *	Wrapped in LOG_COLOR_RED ... LOG_COLOR_RESET. Goes to the same
	 *	sink as LogInfo (default stdout) -- there is no auto-routing
	 *	to stderr.
	*/
	#define LogError(format, args...)                                                                  \
		GenericLogMessage(                                                                             \
		    Log::ERROR,                                                                                \
		    LOG_COLOR_RED LOG_LEVEL_PREFIX_ERROR format LOG_COLOR_RESET,                               \
		    ##args                                                                                     \
		)

	/**
	 * Log a printf-style warning message (Log::WARNING)
	 * @ingroup log
	*/
	#define LogWarning(format, args...)                                                                \
		GenericLogMessage(                                                                             \
		    Log::WARNING,                                                                              \
		    LOG_COLOR_YELLOW LOG_LEVEL_PREFIX_WARNING format LOG_COLOR_RESET,                          \
		    ##args                                                                                     \
		)

	/**
	 * Log a printf-style success message (Log::SUCCESS)
	 * @ingroup log
	*/
	#define LogSuccess(format, args...)                                                                \
		GenericLogMessage(                                                                             \
		    Log::SUCCESS,                                                                              \
		    LOG_COLOR_GREEN LOG_LEVEL_PREFIX_SUCCESS format LOG_COLOR_RESET,                           \
		    ##args                                                                                     \
		)

	/**
	 * Log a printf-style info message (Log::INFO)
	 * @ingroup log
	 *
	 *	No colour wrapper -- INFO is plain. Just the (optional) prefix.
	*/
	#define LogInfo(format, args...) GenericLogMessage(Log::INFO, LOG_LEVEL_PREFIX_INFO format, ##args)

	/**
	 * Log a printf-style verbose message (Log::VERBOSE)
	 * @ingroup log
	*/
	#define LogVerbose(format, args...)                                                                \
		GenericLogMessage(Log::VERBOSE, LOG_LEVEL_PREFIX_VERBOSE format, ##args)

	/**
	 * Log a printf-style debug message (Log::DEBUG)
	 * @ingroup log
	 *
	 *	Off by default -- DEBUG requires explicit `--log-level=debug` or
	 *	`--debug`, since DEFAULT == VERBOSE which is one rung less verbose.
	*/
	#define LogDebug(format, args...)                                                                  \
		GenericLogMessage(Log::DEBUG, LOG_LEVEL_PREFIX_DEBUG format, ##args)

	///////////////////////////////////////////////////////////////////
	/// @name Logging Internals
	/// @internal
	/// @ingroup log
	///////////////////////////////////////////////////////////////////

	///@{

	/*
	 *	ANSI colour escapes used by the colour-bearing log macros.
	 *	Reference: https://misc.flogisoft.com/bash/tip_colors_and_formatting
	 *
	 *	Compile-time switch: -DLOG_DISABLE_COLORS replaces every code
	 *	with the empty string, which is what you want for non-tty sinks
	 *	(file, pipe) where the escapes show up as garbage. There is no
	 *	runtime detection -- it's all or nothing per build.
	*/
	#ifdef LOG_DISABLE_COLORS
		#define LOG_COLOR_RESET ""
		#define LOG_COLOR_RED ""
		#define LOG_COLOR_GREEN ""
		#define LOG_COLOR_YELLOW ""
		#define LOG_COLOR_BLUE ""
		#define LOG_COLOR_MAGENTA ""
		#define LOG_COLOR_CYAN ""
		#define LOG_COLOR_LIGHT_GRAY ""
		#define LOG_COLOR_DARK_GRAY ""
	#else
		// https://misc.flogisoft.com/bash/tip_colors_and_formatting
		#define LOG_COLOR_RESET "\033[0m"
		#define LOG_COLOR_RED "\033[0;31m"
		#define LOG_COLOR_GREEN "\033[0;32m"
		#define LOG_COLOR_YELLOW "\033[0;33m"
		#define LOG_COLOR_BLUE "\033[0;34m"
		#define LOG_COLOR_MAGENTA "\033[0;35m"
		#define LOG_COLOR_CYAN "\033[0;36m"
		#define LOG_COLOR_LIGHT_GRAY "\033[0;37m"
		#define LOG_COLOR_DARK_GRAY "\033[0;90m"
	#endif

	/*
	 *	Per-level text prefixes prepended to every log line.
	 *
	 *	Off by default (empty strings). Define LOG_ENABLE_LEVEL_PREFIX
	 *	at build time to get [E]/[W]/[S]/[I]/[V]/[D] markers, which
	 *	survive piping to a non-tty unlike the colour codes.
	*/
	#ifdef LOG_ENABLE_LEVEL_PREFIX
		#define LOG_LEVEL_PREFIX_ERROR "[E]"
		#define LOG_LEVEL_PREFIX_WARNING "[W]"
		#define LOG_LEVEL_PREFIX_SUCCESS "[S]"
		#define LOG_LEVEL_PREFIX_INFO "[I]"
		#define LOG_LEVEL_PREFIX_VERBOSE "[V]"
		#define LOG_LEVEL_PREFIX_DEBUG "[D]"
	#else
		#define LOG_LEVEL_PREFIX_ERROR ""
		#define LOG_LEVEL_PREFIX_WARNING ""
		#define LOG_LEVEL_PREFIX_SUCCESS ""
		#define LOG_LEVEL_PREFIX_INFO ""
		#define LOG_LEVEL_PREFIX_VERBOSE ""
		#define LOG_LEVEL_PREFIX_DEBUG ""
	#endif

	///@}

#endif
