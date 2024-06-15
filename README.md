# Argentum Programming Language

See: [Project site](https://aglang.org)

## Main language features:

* Safety - memory-safety, type-safety, null-safety, const-safety etc. Unlike Rust and Swift Argetnum has no unsafe mode, and doesn't need one
* Fully automated memory management with no memory leaks. Rust, Swift, arbage-collected languages leak memory. Argentum doesn't
* High efficiency. Argentum is about as fast as C++ and Rust
  * Argentum doesn't use GC (no pauses, no memory and CPU overheads)
  * It compiles to tiny native executables with no extra dependencies.
  * Fast interface method calls
  * Fast dynamic casts
* Simplicity
* Multithreading without deadlocks and data races
* Designed for large apps
  * Modularity
  * Versioning (TBD)
  * Built-in unit tests (TBD)
* Strict type system
* Direct interop with C.

## Links

* Site: [aglang.org](aglang.org)
* Playground: [runs on my RaspberryPi](http://lat.asuscomm.com:3000/)
* Presentation slides: [here](https://docs.google.com/presentation/d/1Cqbh30gTnfoFL3xJh3hhW4Hqhdk9tHw4akZExtiSivA/edit?usp=share_link)

## Examples

This language is "managed", like Java but safer.\
These examples compile into less than 20K Windows executables.\
No virtual machine/framework needed.\
Apps can work forever, no GC pauses, no leaks, no memory corruptions.

#### Hello world

```Rust
sys_log("Hello World")
```

#### SqLite to HTML

```Rust
using sys { log }
using sqliteFfi { Sqlite, xRW }
using string { htmlEncode }

Sqlite.open("mydb.sqlite", xRW) ?
    _.query("
        SELECT "id", "name", "avatar"
        FROM "table"
    ", 0)
    .execute `r log("{}\
        <li id={r.intAt(0)}>
          <img src='ava/{r.stringAt(2)}'/>
          <div>{r.stringAt(1).htmlEncode()}</div>
        </li> 
    ")
```

#More examples are in the [playground](http://lat.asuscomm.com:3000/) and [demo](https://github.com/karol11/argentum/tags).

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

Argentum uses *neither of these approaches*. It distinguishes composition-aggregation-association pointers at syntax level and uses this information:

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
* Standard container library
* Port to Linux x86/64, ARM64
* Bindings to Curl, SqLite.

Within the next couple of months I plan to extend the language:

* Port to: Wasm, Android
* Bindings to Skia
* Standard UI library

I plan to work alone, though I'd appreciate any feedback and bugfixes in the form of pull requests.
