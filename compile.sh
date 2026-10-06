#!/bin/bash

set -euo pipefail

CFLAGS="-std=c17 -Wall -Wextra -Werror -pedantic -Iinclude -O2"

BINARY_NAME="stge"
BUILD_DIR="build"
SRC_FILE=".sources"
BINARY_ARGS=""

function _build_exe()
{
	echo ":: Building '$BINARY_NAME' ..."
	if [ ! -f "$SRC_FILE" ]; then echo -e "error no source file. add a file with $0 add src/file.c" && exit; fi
	if [ ! -d "$BUILD_DIR" ]; then mkdir "$BUILD_DIR" && echo ":: created build dir"; fi
	gcc $CFLAGS $(cat "$SRC_FILE") -o "$BUILD_DIR/$BINARY_NAME"
}

function _check_script()
{
	echo ":: Checking script language ..."
	"./$BUILD_DIR/$BINARY_NAME" -game script_test -check
	echo ":: script_test ok"
}

function _init()
{
	if [ ! -d "src" ]; then
		mkdir -p src
		echo ":: created src/"
	fi
	if [ ! -d "include" ]; then
		mkdir -p include
		echo ":: created include/"
	fi

	if [ ! -f "src/main.c" ]; then
		cat > src/main.c <<'EOF'
int main(void)
{
}
EOF
		echo ":: created src/main.c"
	fi

	touch "$SRC_FILE"
	if ! grep -Fxq "src/main.c" "$SRC_FILE"; then
		printf '%s\n' "src/main.c" >> "$SRC_FILE"
		echo ":: added src/main.c"
	fi
}


if [ $# -eq 0 ]; then
	_build_exe
	_check_script
else
	case "$1" in
	"run")
		_build_exe
		echo ":: running $BINARY_NAME"
		shift
		BINARY_ARGS=$@
		set +e
		./$BUILD_DIR/$BINARY_NAME $@
		__exit_code=$?
		set -e
		echo ":: exit($__exit_code): $BINARY_NAME $BINARY_ARGS"
		exit $__exit_code
		;;
	"clean")
		if [ -d "build" ]; then 
			rm -rf "./build"
			echo ":: Cleaned build directory ..."
		fi
		;;
	"add")
		touch "$SRC_FILE"

		shift

		if [ $# -eq 0 ]; then
			echo "usage: $0 add <files/globs>"
			exit 1
		fi
		for file in "$@"; do
			if [ -d "$file" ]; then
				echo "ignoring $file/"
				continue
			fi

			if [ ! -f "$file" ]; then
				echo "$file doesnt exist"
				continue
			fi

			if grep -Fxq "$file" "$SRC_FILE"; then
				echo "$file already present"
				continue
			fi

			printf '%s\n' "$file" >> "$SRC_FILE"
			echo "added $file"
		done

		;;
	"remove")
		if [ ! -f "$SRC_FILE" ]; then
			echo "no source file list"
			exit 1
		fi

		shift

		if [ $# -eq 0 ]; then
			echo "usage: $0 remove <files/globs>"
			exit 1
		fi

		for file in "$@"; do
			if [ -d "$file" ]; then
				echo "ignoring $file/"
				continue
			fi

			if ! grep -Fxq "$file" "$SRC_FILE"; then
				echo "$file not present"
				continue
			fi

			echo "removing $file"

			tmp_file="$(mktemp)"

			grep -Fxv "$file" "$SRC_FILE" > "$tmp_file"

			mv "$tmp_file" "$SRC_FILE"
		done
		;;	
	"sources")
		if [ -f "$SRC_FILE" ]; then
			cat "$SRC_FILE"
		fi
		;;
	"test")
		_build_exe
		_check_script
		;;
	"config")
		if [ -f "$SRC_FILE" ]; then
			echo ":: source files"
			cat "$SRC_FILE"
			echo ":: gcc flags"
			echo "$CFLAGS"
		fi
		;;
	"help"|"-help"|"h"|"-h")
		echo ":: available commands"
		echo " init	- creates scaffold project"
		echo " run	- builds and runs the project and passes args"
		echo " test	- builds and checks script_test"
		echo " add	- adds a source file to the build config"
		echo " remove	- removes a source file from the build config"
		echo " sources  - displays source file inputs"
		echo " config   - displays cflags and source file inputs"
		;;
	"init")
		_init
		;;
	*)
		echo ":: Unknown option '$1'"
		exit 1
		;;
	esac
fi

