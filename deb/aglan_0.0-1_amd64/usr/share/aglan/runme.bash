#!/bin/bash
YELLOW='\033[1;33m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color
if [ "$#" -eq 0 ]; then
    dir_name=$(mktemp -d)
    echo "Creating temporary directory: $dir_name"
    echo "Call this script with a directory name to use it istead."
else
    dir_name="$1"
    if [ -d "$dir_name" ]; then
        echo "Directory '$dir_name' already exists. Content will be reused."
    else
        if mkdir "$dir_name"; then
            echo "Directory $dir_name created"
        else
            echo "Cant create directory $dir_name"
            exit $?
        fi
    fi
fi
pushd "$dir_name" > /dev/null
cp -n /usr/share/aglan/demo-src/* .

demo_list=("helloWorld" "fizzBuzz" "bottles" "threadTest" "graph" "demo")
for demo in "${demo_list[@]}"; do
    echo -e "${YELLOW}================ $demo ==================${GREEN}"
    cat "$demo.ag"
    echo -e "${NC}"
    read -p "Press Enter to run..."
    agc -src . -start "$demo" -o "$demo.o"
    gcc -no-pie "$demo.o" /usr/lib/aglan/libag_runtime.a -L/usr/lib/x86_64-linux-gnu -lSDL2 -lSDL2_image -o "$demo"
    echo -e "${GREEN}"
    "./$demo"
    echo -e "${NC}"
    read -p "Press Enter to continue..."
done
popd > /dev/null
echo -e "${YELLOW}============= That's all folks ================${NC}"
echo -e "for more info see ${GREEN}/usr/share/aglan/readme.md${NC}"

if [ "$#" -eq 0 ]; then
    echo "Removing temporary directory: $dir_name"
    rm -r "$dir_name"
fi
