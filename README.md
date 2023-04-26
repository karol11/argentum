# Argentum Programming Language

See: [Project site](aglang.org)

## Main language features:

* Simplicity
* Syntax driven rock-solid safety
* Fully automated memory management (no memory leaks)
* High efficiency
  * No GC (no pauses, no memory and CPU overhead)
  * AOT
  * Fast interface method calls
  * Fast dynamic casts
* Designed for large apps
  * Modularity
  * Versioning
  * Built-in unit tests
* Strict type system
* Tiny runtime (it is designed with wasm in mind)

## Links
* Site: [aglang.org](aglang.org)
* Hackaday: [project page](https://hackaday.io/project/190397-argentum-programming-language)
* Presentation slides: [here](https://docs.google.com/presentation/d/1Cqbh30gTnfoFL3xJh3hhW4Hqhdk9tHw4akZExtiSivA/edit?usp=share_link)
* Documentaion: [here](https://docs.google.com/document/d/1QCvxUGr2kce67jht8PLH822ZuZSXHvMIFgsACsbV4Y4/edit?usp=sharing)

## Examples

#### Fizz-buzz

```Rust
using sys { String; log; }
using string { Builder; }
using utils{ forRange; }

b = Builder;
forRange(1, 101, (i){
   i % 3 == 0 ? b.putStr("fizz");
   i % 5 == 0 ? b.putStr("buzz");
   b.pos == 0 ? b.putInt(i);
   log(b.newLine().toStr());
});
```

#### Find loop in graph

```Rust
using sys { WeakArray; Array; String; log; }
using utils { existsInRange; }
using string;
using array;

class Node {
    connections = WeakArray;
    isVisited = false;
    isActive = false;
    hasLoop() bool {
        isActive || (!isVisited && {
            isVisited := isActive := true;
            r = connections.contain((c){ c~Node && _.hasLoop() });
            isActive := false;
            r
        })
    }
}
class Graph {
    nodes = Array;
    fromStr(String in) this {
        byNames = WeakArray.resize('z' + 1);
        getOrCreateNode = () {
            name = in.get();
            byNames[name] || byNames[name] := nodes.append(Node)
        };
        loop {
            from = getOrCreateNode();
            in.get();
            from ? _~Node ? _.connections.append(getOrCreateNode());
            in.get() == 0
        }
    }    
    hasLoop() bool {
        nodes.contain((n){ n~Node && _.hasLoop() })
    }
}
log(Graph.fromStr("a>b b>c c>d e>f b>e e>a c>c").hasLoop()
    ? "has some loop"
    : "has no loops");
```

## Why not X

There are many programming languages in the wild.

Some of them make programmer to manually allocate and free memory:

* C
* C++ (if not using smart pointers)
* ADA
* Pascal etc.

Other languages automate this process using mark/sweep or copying Garbage Collector. It simplifies language and ease the programmers life but at the cost. GC-driven languages are slower, they consume more memory (sometimes twice) but what's worse, they always pause applications at unpredictable moments for unpredictable periods. These languages are:

* Java
* Go
* Kotlin
* Java Script
* Dart etc.

There are third group of languages, that use semi-automatic approach usualy based on ref-counting. Programmer don't have to think too much on memory management, but their main downside is: memory leaks are possible and in fact without strict discipline they are inevidable. These languages are:

* Swift
* Rust
* C++ (with smart pointers) etc.

Argentum uses neither of these approaches. It distinguishes composition-aggregation-association pointers at syntax level and uses this information:

* To automate most routine operations on data structures
* To check the correctness of operations on data structures at compile time
* To automate lifetime management of object lifetimes, that not only protects from memory leaks but allow to manage all system and hardware resources in a straightforward way.

Let's say it this way: instead of allowing programmers to make a mess out of their data structures (and spend computer resources on handling this mess), Argentum keeps up its data structures in perfect condition all the way.
So far no other programming language can do so.

## Development

I have a working prototype of the first milestone.
Within the next couple of months I plan to extend the language and make my JIT compiler an AOT one targeted on every platfor supported by LLVM/SDL.

I plan to work alone, though I'd appreciate any feedback and bugfixes in the form of pull requests.
