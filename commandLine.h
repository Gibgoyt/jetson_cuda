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

/*
 *	commandLine.h
 *	argv parser used by every entry point for the jetson-inference tool
 *
 *	pipeline stage 1
 *		```c/c++
 * 		int main(int argc, char** argv) {
 * 		    commandLine cmdLine(argc, argv, IS_HEADLESS());
 * 		    ...
 * 		    const char* model = cmdLine.GetString("model");
 * 		    const float threshold = cmdLine.GetFloat("threshold", 0.5f);
 * 		    const char* input  = cmdLine.GetPosition(0);
 * 		    const char* output = cmdLine.GetPosition(1);
 * 		}
 * 		```
 *
 * 		so this file being the application boundary converting any argv into typed values that the rest of the program can use
 *	Parsing model:
 *		- Named args use `--name=value` or `--name value`-adjacent forms.
 *		  (See commandLine.cpp for the exact scan rules; named-arg lookup is
 *		   a case-insensitive *prefix* match, while GetFlag is an exact match.)
 *		- Positional args are everything in argv that does NOT start with `-`.
 *		  They are referenced by zero-based index via GetPosition(i).
 *		- Delimiter equivalence: by default, `-` and `_` are treated as
 *		  interchangeable inside an argName, so `--foo-bar` matches `--foo_bar`.
 *		  Disable by passing `allowOtherDelimiters = false` to a getter.
 *
 *	Side-effect to remember:
 *		Both constructors call `Log::ParseCmdLine(*this)`, which inspects flags
 *		like `--log-level=...` and `--verbose` and reconfigures the global
 *		logger. Merely constructing a commandLine therefore initializes logging
 *		-- that's why every main() builds one of these very early.
 *
 *	Dependencies:
 *		- <stdlib.h>, <stdint.h>  (uint32_t)
 *		- logging.h (only from the .cpp, for the constructor's logging hook)
*/

#if !defined(__COMMAND_LINE_H_)
	#define __COMMAND_LINE_H_
	
	#include <stdlib.h>
	#include <stdint.h>
	
		/**
		 * Command line parser for extracting flags, values, and strings.
		 * @ingroup commandLine
		 */
		class commandLine {
		public:
			/*
			 *	Construction
			 *
			 *	Both constructors store argc/argv by reference (no deep copy) and
			 *	invoke Log::ParseCmdLine internally, so building this object also
			 *	configures the global logger.
			*/

			/**
			 * Constructor, takes the command line from `main()`
			*/
			commandLine(
				const int argc, 
				char** argv, 
				const char* extraFlag = NULL
			);
	
			/**
			 * Constructor, takes the command line from `main()`
			 */
			commandLine(
				const int argc, 
				char** argv, 
				const char** extraArgs
			);
	
			/*
			 *	--- Named-argument lookups (typed) -----------------------------------
			 *	All four getters share one scan pattern (see commandLine.cpp): walk
			 *	argv, find the leading `-`/`--`, prefix-compare against argName, and
			 *	on hit parse the substring after `=` (or treat as 0/empty if there
			 *	is no `=`). On miss, optionally retry with `-` <-> `_` swapped.
			*/

			/**
			 * Checks to see whether the specified flag was included on the
			 * command line.   For example, if argv contained `--foo`, then
			 * `GetFlag("foo")` would return `true`
			 *
			 * @param allowOtherDelimiters if true (default), the argName will be
			 *          matched against occurances containing either `-` or `_`.
			 *          For example, `--foo-bar` and `--foo_bar` would be the same.
			 *
			 * @returns `true`, if the flag with argName was found
			 *          `false`, if the flag with argName was not found
			 *
			 *	NOTE: unlike the other named-arg getters below, GetFlag uses an
			 *	      *exact* match (it compares length-equal tokens up to an `=`),
			 *	      so `--foobar` will NOT satisfy `GetFlag("foo")`.
			*/
			bool GetFlag(
				const char* argName, 
				bool allowOtherDelimiters = true
			) const;
	
			/**
			 * Get float argument.  For example if argv contained `--foo=3.14159`,
			 * then `GetInt("foo")` would return `3.14159f`
			 *
			 * @param allowOtherDelimiters if true (default), the argName will be
			 *          matched against occurances containing either `-` or `_`.
			 *          For example, `--foo-bar` and `--foo_bar` would be the same.
			 *
			 * @returns `defaultValue` if the argument couldn't be found. (`0.0` by default).
			 *          Otherwise, returns the value of the argument.
			 */
			float GetFloat(
			    const char* argName,
			    float defaultValue = 0.0f,
			    bool allowOtherDelimiters = true
			) const;
	
			/**
			 * Get integer argument.  For example if argv contained `--foo=100`,
			 * then `GetInt("foo")` would return `100`
			 *
			 * @param allowOtherDelimiters if true (default), the argName will be
			 *          matched against occurances containing either `-` or `_`.
			 *          For example, `--foo-bar` and `--foo_bar` would be the same.
			 *
			 * @returns `defaultValue` if the argument couldn't be found (`0` by default).
			 *          Otherwise, returns the value of the argument.
			 */
			int GetInt(
				const char* argName, 
				int defaultValue = 0, 
				bool allowOtherDelimiters = true
			) const;
	
			/**
			 * Get unsigned integer argument.  For example if argv contained `--foo=100`,
			 * then `GetUnsignedInt("foo")` would return `100`
			 *
			 * @param allowOtherDelimiters if true (default), the argName will be
			 *          matched against occurances containing either `-` or `_`.
			 *          For example, `--foo-bar` and `--foo_bar` would be the same.
			 *
			 * @returns `defaultValue` if the argument couldn't be found, or if the value
			 *          was negative (`0` by default). Otherwise, returns the parsed value.
			 *
			 *	Wrapper around GetInt that clamps negative parse results back to
			 *	defaultValue (so callers can rely on a non-negative result).
			*/
			uint32_t GetUnsignedInt(
			    const char* argName,
			    uint32_t defaultValue = 0,
			    bool allowOtherDelimiters = true
			) const;
	
			/**
			 * Get string argument.  For example if argv contained `--foo=bar`,
			 * then `GetString("foo")` would return `"bar"`
			 *
			 * @param allowOtherDelimiters if true (default), the argName will be
			 *          matched against occurances containing either `-` or `_`.
			 *          For example, `--foo-bar` and `--foo_bar` would be the same.
			 *
			 * @returns `defaultValue` if the argument couldn't be found (`NULL` by default).
			 *          Otherwise, returns a pointer to the argument value string
			 *          from the `argv` array.
			 *
			 *	The returned pointer aliases into argv -- it is NOT a fresh copy,
			 *	so its lifetime is the lifetime of argv (i.e. the whole program for
			 *	args that came from main(), or the lifetime of this object for args
			 *	added via AddArg/AddArgs/AddFlag, which allocate).
			 */
			const char* GetString(
			    const char* argName,
			    const char* defaultValue = NULL,
			    bool allowOtherDelimiters = true
			) const;
	
			/*
			 *	--- Positional arguments ---------------------------------------------
			 *	"Positional" = any argv entry that does NOT start with `-`.
			 *	These getters skip argv[0] (the program name); see commandLine.cpp.
			*/

			/**
			 * Get positional string argument.  Positional arguments aren't named, but rather
			 * referenced by their index in the list. For example if the command line contained
			 * `my-program --foo=bar /path/to/my_file.txt`, then `GetString(0)` would return
			 * `"/path/to/my_file.txt"
			 *
			 * @returns `defaultValue` if the argument couldn't be found (`NULL` by default).
			 *          Otherwise, returns a pointer to the argument value string
			 *          from the `argv` array.
			*/
			const char* GetPosition(
				unsigned int position, 
				const char* defaultValue = NULL
			) const;
	
			/**
			 * Get the number of positional arguments in the command line.
			 * Positional arguments are those that don't have a name.
			*/
			unsigned int GetPositionArgs() const;

			/*
			 *	--- Mutating the parsed command line ---------------------------------
			 *	These grow argv at runtime by allocating a larger array and copying
			 *	the existing pointers. Useful for synthesizing args (e.g. the
			 *	`extraFlag` ctor parameter funnels through AddFlag).
			*/
	
			/**
			 * Add an argument to the command line.
			 */
			void AddArg(
				const char* arg
			);
	
			/**
			 * Add arguments to the command line.
			 */
			void AddArgs(
				const char** args
			);
	
			/**
			 * Add a flag to the command line.
			 *
			 *	AddFlag prepends `--` for you and is a no-op if the flag is already
			 *	present (uses GetFlag to dedupe).
			 */
			void AddFlag(const char* flag);
	
			/*
			 *	--- Misc -------------------------------------------------------------
			*/

			/**
			 * Print out the command line for reference.
			 */
			void Print() const;
	
			/*
			 *	--- Raw argc/argv passthrough ----------------------------------------
			 *	Exposed publicly because some callers (notably Log::ParseCmdLine and
			 *	other utils) want to re-scan the args themselves. Mutated by the
			 *	AddArg/AddArgs/AddFlag methods above.
			*/

			/**
			 * The argument count that the object was created with from main()
			 */
			int argc;
	
			/**
			 * The argument strings that the object was created with from main()
			 */
			char** argv;
		};
	
		/**
		 * Specify a positional argument index.
		 * @ingroup commandLine
		 * 
		 *	ARG_POSITION is a documentation no-op: it expands to its argument unchanged.
		 *	It exists purely to make call sites self-describing, e.g.:
		 *	    const char* in  = cmdLine.GetPosition(ARG_POSITION(0));   // input path
		 *	    const char* out = cmdLine.GetPosition(ARG_POSITION(1));   // output path
		 *	reads more clearly than passing a bare `0` / `1`.
		*/
	#define ARG_POSITION(x) x
	
#endif
