# Argentum Programming Language

## Links

* Site: [https://aglang.org](aglang.org)
* Playground: [runs on my RaspberryPi](http://lat.asuscomm.com:3000/)

## Main language features:

* Safety - memory-safety, type-safety, null-safety, const-safety etc.\
  Unlike Rust and Swift Argetnum has no unsafe mode and doesn't need one
* Fully automated memory management with no memory leaks.\
  Rust, Swift, garbage-collected languages leak memory; Argentum doesn't
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
using string { htmlEscape }

Sqlite.open("mydb.sqlite", xRW) ? `db
    db.query("
        SELECT "id", "name", "avatar"
        FROM "table"
    ", 0)
    .execute `row {
        log("{}\
           <li id={row.intAt(0)}>
              <img src="ava/{row.stringAt(2)}"/>
              <div>{row.stringAt(1).htmlEscape()}</div>
           </li> 
        ")
    }
```

#More examples: [playground](http://lat.asuscomm.com:3000/), [demo](https://github.com/karol11/argentum/tags).

## Why not X

* Java, Go, Kotlin, Java Script, Dart, C# etc.
  * using Garbage collector
  * Have unpredictable pauses
  * Have huge memory and CPU overheads
  * Create hard to detect memory leaks
  * Have heavy VMs/Runtimes/Frameworks
* Rust, Swift etc
  * Built on ref-counting
  * Have unsafe mode and force developers to use this mode by disallowing the very basic operations
  * prone to hard to detect memory leaks caused by cycles in ownership graphs
* C, C++ etc.
  * Manual memory management
  * Leaks
  * Permanent Unsafe mode.

## Development

I have a working prototype of the 2nd milestone that includes:

* Stand-alone compiler producing windows and Linux execitables for X86 and ARM64
* Multithreading
* Parameterized classes and interfaces
* String interpolation
* Frozen-mutable object hierrarchies
* Fast unwind and direct `breaks` from nested levels of lambdas
* Standard container library
* Bindings to Curl, SqLite, SDL.

Within the next couple of months I plan to extend the language:

* Port to: Wasm, Android
* Bindings to Skia
* Standard UI library

I plan to work alone, though I'd appreciate any feedback and bugfixes in the form of pull requests.
