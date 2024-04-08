# Argentum Programming Language

See: [Project site](https://aglang.org)

## Main language features:

* Simplicity
* Syntax driven rock-solid safety
* Fully automated memory management (no memory leaks)
* High efficiency
  * No GC (no pauses, no memory and CPU overhead)
  * AOT Compilation
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

## Examples

This language is safe and managed, like Java.\
These code examples compile into less than 20K Windows executables.\
No virtual machine/framework needed.\
They can work in a loop forever, no GC pauses, no leaks, no memory corruptions.

#### Fizz-buzz

```Rust
using sys { String, log }
using string { Builder }
using utils{ forRange }

b = Builder;
forRange(1, 101) {
   _ % 3 == 0 ? b.putStr("fizz");
   _ % 5 == 0 ? b.putStr("buzz");
   b.pos == 0 ? b.putInt(_);
   log(b.newLine().toStr());
}
```

#### Find loop in a graph

```Rust
using sys { WeakArray, Array, String, log }
using utils { existsInRange }
using string;
using array;

class Node {
    connections = WeakArray(Node);
    isVisited = false;
    isActive = false;
    hasLoop() bool {
        isActive || (!isVisited && {
            isVisited := isActive := true;
            r = connections.contain{ _.hasLoop() };
            isActive := false;
            r
        })
    }
}
class Graph {
    nodes = Array(Node);
    fromStr(String in) this {
        byNames = WeakArray.resize('z' + 1);
        getOrCreateNode = \{
            name = in.get();
            byNames[name] || byNames[name] := nodes.append(Node)
        };
        loop {
            from = getOrCreateNode();
            in.get();
            from.connections.append(getOrCreateNode());
            in.get() == 0
        }
    }    
    hasLoop() bool {
        nodes.contain{ _.hasLoop() }
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

There is a third group of languages, that use semi-automatic approach usualy based on ref-counting. Programmer don't have to think too much on memory management, but their main downside is: memory leaks are possible and in fact without strict discipline they are inevidable. These languages are:

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

I have a working prototype of the 2nd milestone that includes:

* Stand-alone compiler producing windows execitables
* Multithreading
* Parameterized classes and interfaces
* String interpolation
* Frozen-mutable object hierrarchies
* Fast unwind and direct `breaks` from nested levels of lambdas

Within the next couple of months I plan to extend the language:

* Standard container library (done: arrays, maps)
* Port to some platforms: Linux (done), Wasm, Android
* Standard UI library
* Bindings to curl (done), databases (done).

I plan to work alone, though I'd appreciate any feedback and bugfixes in the form of pull requests.
