#include <unordered_map>
#include "utils/fake-gunit.h"
#include "dom/dom-to-string.h"
#include "compiler/ast.h"
#include "compiler/parser.h"
#include "compiler/name-resolver.h"
#include "compiler/type-checker.h"
#include "compiler/const-capture-pass.h"
#include "runtime/runtime.h"

int64_t generate_and_execute(ltm::pin<ast::Ast> ast, bool add_debug_info, bool dump_ir);  // defined in `generator.h/cpp`

namespace {

using std::unordered_map;
using std::string;
using std::move;
using ltm::pin;
using ltm::own;
using dom::Name;
using ast::Ast;

int64_t foreign_test_function_state = 0;

int64_t foreign_test_function(int64_t delta) {
    return foreign_test_function_state += delta;
}
void void_void_tramp(AgObject* self, ag_fn entry_point, ag_thread* th) {
    // extract params here
    ag_unlock_thread_queue(th);
    if (self)
        ((void (*)(AgObject*)) entry_point)(self);
    // release params here
}
void callback_invoker(AgWeak* cb_data, ag_fn cb_entry_point) {
    ag_retain_weak(cb_data);
    auto th = ag_prepare_post_message(cb_data, cb_entry_point, void_void_tramp, 0);
    // post params here
    ag_finalize_post_message(th);
}

void ag_assert(int64_t expected, int64_t actual) {
    ASSERT_EQ(expected, actual);
}

void execute(const char* source_text, bool dump_all = false) {
    ast::initialize();
    auto ast = own<Ast>::make();
    ast->platform_exports.insert({ "ag_fn_akTest_foreignTestFunction", (void(*)())(foreign_test_function) });
    ast->platform_exports.insert({ "ag_fn_akTest_callbackInvoker", (void(*)())(callback_invoker) });
    ast->mk_fn("assert", (void(*)())(ag_assert), new ast::ConstVoid, { ast->tp_int64(), ast->tp_int64() });
    auto start_module_name = "akTest";
    unordered_map<string, string> texts{ {start_module_name, source_text} };
    parse(ast, start_module_name, [&](string name) {
        return move(texts[name]);
    });
    resolve_names(ast);
    check_types(ast);
    const_capture_pass(ast);
    if (dump_all)
        std::cout << std::make_pair(ast.pinned(), ast->dom.pinned()) << "\n";
    foreign_test_function_state = 0;
    generate_and_execute(ast, false, dump_all);
}

TEST(Parser, BoolLambda) {
    execute(R"(
        fn f(l()bool) bool { l() }
        sys_assert(1, f((){false}) ? 0:1)
    )");
}

TEST(Parser, Ints) {
    execute("sys_assert(7, (2 ^ 2 * 3 + 1) << (2-1) | (2+2) | (3 & (2>>1)))");
}

TEST(Parser, Doubles) {
    execute("sys_assert(3, int(3.14 - 0.2e-4 * 5.0))");
}

TEST(Parser, Block) {
    execute("sys_assert(3, {1+1}+1)"); 
}

TEST(Parser, Functions) {
    execute(R"-(
      fn plus1(a int) int {
            a + 1
      }
      sys_assert(9, plus1(4) + plus1(3))
    )-");
}

TEST(Parser, LambdaWithParams) {
    execute("sys_assert(40, (a){1+a}(3)*10)");
}

TEST(Parser, PassingLambdaToLambda) {
    execute(R"-( sys_assert(99,
      (a, b, xfn) {
            xfn((t) {
                a + b + t
            })
      } (2, 4, (vfn) {
            vfn(3) * vfn(5)
      }))
    )-");
}

TEST(Parser, Sequecnce) {
    execute("sys_assert(44, {1; 3.14; 44})");
}

TEST(Parser, LocalAssignment) {
    execute("sys_assert(10, {a = 2; a := a + 3; a * 2})");
}

TEST(Parser, MakeAndConsumeOptionals) {
    execute("sys_assert(2, (opt){ opt : 2 } (false ? 44))");
}

TEST(Parser, MaybeChain) {
    execute(R"(sys_assert(3, {
        a = +3.3;  // a: local of type optional(double), having value just(3.3)
        a ? int(_) : 0  // convert optional(double) to optional(int) and extract replacing `none` with 0
    })
    )");
}

TEST(Parser, IntLessThan) {
    execute("sys_assert(3, { a = 2; a < 10 ? 3 : 44 })");
}

TEST(Parser, IntNotEqual) {
    execute("sys_assert(3, { a = 2; a != 10 ? 3 : 44 })");
}

TEST(Parser, Loop) {
    execute(R"(
      using sys { assert }
      a = 0;
      r = 1;
      assert(39916800, loop {
          a := a + 1;
          r  := r * a;
          a > 10 ? r
      })
    )");
}

TEST(Parser, Classes) {
    execute(R"(
        class Point {
          x = 0;
          y = 2;
        }
        p=Point;
        p.x := 1;
        sys_assert(3, p.y + p.x)
    )");
}

TEST(Parser, ClassInstanceCopy) {
    execute(R"(
        class Point {
          x = 0;
          y = 2;
        }
        p=Point;
        p.x := 1;
        pb = @p;
        pb.x := 3;
        sys_assert(4, p.x + pb.x)
    )");
}

TEST(Parser, ClassMethods) {
    execute(R"(
        class Point {
          x = 1;
          y = 2;
          m() int { x+y }
        }
        class P3 {
          +Point{
            m() int { x+y+z }
          }
          z=3;
        }
        p=P3;
        p.x := 10;
        p.z := 20;
        sys_assert(32, p.m())
    )");
}

TEST(Parser, Interfaces) {
    execute(R"(
        interface Movable {
          move(x int, y int);
        }
        class Point {
          x = 1;
          y = 2;
          m() int { x+y }
        }
        class P3 {
          +Point{
            m() int { x+y+z } 
          }
          +Movable {
            move(dx int, dy int) {
              x := x + dx;
              y := y + dy
            }
          }
          z = 3;
        }
        p=P3;         // 1, 2, 3
        p.x := 10;    // 10, 2, 3
        p.z := 20;    // 10, 2, 20
        p.move(2, 3); // 12, 5, 20
        sys_assert(37, p.m())
    )");
}

TEST(Parser, TwoInterfaces) {
    execute(R"(
        interface Movable {
          moveTo(x int, y int);
        }
        interface Sizeable {
          resize(width int, height int);
        }
        class Point {
          x = 0;
          y = 0;
        }
        class Widget {
          +Point;
          +Movable {
            moveTo(x int, y int) {
              this.x := x;
              this.y := y;
            }
          }
          +Sizeable {
            resize(width int, height int) {
              size.x := width;
              size.y := height;
            }
          }
          size = Point;
        }
        p = Widget;
        p.moveTo(1,2);
        p.resize(100,200);
        sys_assert(201, p.x + p.size.y)
    )");
}

TEST(Parser, PromisedCast) {
    execute(R"(
        interface Movable {
          moveTo(x int, y int);
        }
        class Point {
          +Movable {
            moveTo(x int, y int) {
              this.x := x;
              this.y := y;
            }
          }
          x = 0;
          y = 0;
        }
        class Widget {
          +Movable {
            moveTo(x int, y int) {
              pos.moveTo(x, y);
            }
          }
          pos = Point;
        }
        p = Widget~Movable;
        p.moveTo(1, 2);
        p := Point;
        p.moveTo(10, 20)
    )");
}

TEST(Parser, ClassCast) {
    execute(R"(
        class Point {
          x = 0;
          y = 0;
        }
        class Widget {
          +Point;
          color = 24;
        }
        p = Widget~Point;  // `p` is a `Point` holder initialized with a `Widget` instance
        sys_assert(24, p~Widget?_.color : 0)     // cast `p` to `Widget` and return its `color` field on success
    )");
}

TEST(Parser, InterfaceCast) { // TODO interface dispatch with collisions
    execute(R"(
        interface Opaque {
          bgColor() int;
        }
        class Point {
          x = 0;
          y = ~0;
        }
        class Widget {
          +Point;
          +Opaque { bgColor() int { color } }
          color = 7;
        }
        p = Point;
        w = Widget~Point;
        a = p~Opaque?_.bgColor() : 40;  // expected to fallback to 40
        b = w~Opaque?_.bgColor() : 50;  // expected to return 7
        sys_assert(47, a + b)
    )");
}

TEST(Parser, Weak) {
    execute(R"(
        class Point {
          x = 0;
        }
        p = Point;
        w = &p;
        p.x := 22;
        r = w?_.x : 100;
        p := Point;
        w ? r := r + _.x;
        sys_assert(22, r)
    )");
}

TEST(Parser, TopoCopy) {
    execute(R"(
        class Node {
          parent = &Node;  // Weak(Node) = null
          left = ?Node;    // Optional(own(Node)) = null
          right = ?Node;

          scan(expectedParent &Node) int {
            lcount = left?_.scan(&this) : 0;
            rcount = right?_.scan(&this) : 0;
            this.parent == expectedParent
                ? lcount + rcount + 1
                : -100
          }
        }
        root = Node;
        root.left := Node;
        root.right := Node;
        root.left?_.parent := &root;
        root.right?_.parent := &root;

        oldSize = root.scan(&Node);

        root.left := @root;
        root.left?_.parent := &root;

        sys_assert(35, oldSize * 10 + root.scan(&Node))
    )");
}

TEST(Parser, ForeignFunctionCall) {
    execute(R"(
        fn foreignTestFunction(x int) int;
        foreignTestFunction(4*10);
        sys_assert(42, foreignTestFunction(2))
    )");
}

TEST(Parser, LogicalOr) {
    execute(R"(
        a = 3;
        sys_assert(42, a > 2 || a < 4 ? 42 : 0)
    )");
}

TEST(Parser, LogicalAnd) {
    execute(R"(
        a = 3;
        sys_assert(42, a > 2 && a < 4 ? 42 : 0)
    )");
}

TEST(Parser, Raii) {
    execute(R"(
        class Font {
            ttfHandle = 0;
            setId(id int) {
                ttfHandle != 0 ? foreignTestFunction(-ttfHandle);
                ttfHandle := id;
                id != 0 ? foreignTestFunction(id);
            }
        }
        fn disposeFont(f Font) {
            f.ttfHandle != 0 ? foreignTestFunction(-f.ttfHandle);
        }
        fn afterCopyFont(f Font) {
            f.ttfHandle != 0 ? foreignTestFunction(f.ttfHandle);
        }
        fn foreignTestFunction(x int) int;

        {
            foreignTestFunction(2);
            fa = Font;
            fa.setId(42);   // foreignTestFunction_state= 2+42
            fb = @fa;       // foreignTestFunction_state= 2+42+42
        };                  // foreignTestFunction_state= 2+42+42-42-42 = 2
        sys_assert(2, foreignTestFunction(0))
    )");
}

TEST(Parser, BlobsAndIndexes) {
    execute(R"(
        using sys {
            Blob,
            assert
        }
        class sys_Blob {
            getAt(i int) int { get64At(i) }
            setAt(i int, v int) { set64At(i, v) }
        }
        b = Blob;
        b.insertItems(0, 3);
        b[1] := 42;
        c = @b;
        assert(42, c[1])
    )");
}

TEST(Parser, Delegates) {
    execute(R"(
        class Cl{
            x = 11;
            m(i int) int { x + i }
        }
        fn f(handler &(int)int) int {
            handler(42) : 0
        }
        c = Cl;
        sys_assert(53, f(c.m));
        sys_assert(31, f(c.&diff(i int) int { i - x }))
    )");
}

TEST(Parser, Arrays) {
    execute(R"(
        class Node {
            x = 1;
            y = 0;
        }
        a = sys_Array(Node);
        a.insertItems(0, 10);
        a[0] := Node;
        a[1] := Node;
        a[1] ? {
            _.x := 42;
            _.y := 33;
        };
        c = @a;
        c.delete(0, 1);
        sys_assert(42, c[0] ? _.x : -1)
    )");
}

TEST(Parser, WeakArrays) {
    execute(R"(
        class Node {
            x = 1;
            y = 0;
        }
        n = Node;
        a = sys_WeakArray(Node);
        a.insertItems(0, 10);
        a[0] := &n;
        a[1] := &n;
        c = @a;
        c.delete(0, 1);
        sys_assert(1, c[0] && _==n ? 1:0)
    )");
}

TEST(Parser, RetOwnPtr) {
    execute(R"(
        class Node {
            x = 1;
            y = 0;
        }
        fn nodeAt(x int, y int) @Node {  // Callables can return newly created/copied object
            r = Node;
            r.x := x;
            r.y := y;
            r
        }
        a = sys_Array(Node);
        a.insertItems(0, 1);
        a[0] := nodeAt(42, 33);  // no copy here!
        sys_assert(42, a[0] ? _.x : -1)
    )");
}

TEST(Parser, InitializerMethods) {
    execute(R"(
        class Node {
            x = 1;
            y = 0;
            flags = 0;
            initPos(x int, y int) this {  // Functions marked with this when called on @pointer return @pointer, and with correct derived type!
                this.x := x;
                this.y := y;
            }
            transpose() this {
                t = this.x;
                this.x := this.y;
                this.y := t;
            }
            addFlags(f int) this {
                this.flags := this.flags | f;
            }
            removeFlags(f int) this {
                this.flags := this.flags & ~f;
            }
        }
        class Bar {
            n = Node             // 1 0 0    // they can be used as rich combinable, named, virtual constructors
                .initPos(1, 4)   // 1 4 0
                .transpose()     // 4 1 0
                .addFlags(3);    // 4 1 3
        }
        b = Bar;
        b.n.removeFlags(2);      // 4 1 1    // they can be used as normal methods too
        sys_assert(5, b.n.x + b.n.flags)         // 5
    )");
}

TEST(Parser, StringOperations) {
    execute(R"(
        class sys_String{
            get() int { getCh() }
            length() int {
                r = 0;
                loop {
                    c = getCh();
                    c != 0 ? r := r + 1;
                    c == 0 ? r
                }
            }
        }
        class OString {
            +sys_Blob;
            pos = 0;
            put(codePoint int) this {
                size = capacity();
                growStep = 100;
                pos + 5 >= size ?
                    insertItems(size, growStep);
                pos := putChAt(pos, codePoint)
            }
            append(s sys_String) this {
                loop{
                    c = s.getCh();
                    c != 0 ? put(c);
                    c == 0
                }
            }
            str() @sys_String {
                r = sys_String;
                r.fromBlob(this, 0, pos);
                pos := 0;
                r
            }
        }
        a = OString.put('<').append("Hello there").put('>').str();
        sys_assert(13, a.length())
    )");
}

TEST(Parser, LiteralStrings) {
    execute(R"(
        a = "Hi";
        a.getCh(); // a="i"
        b = @a;
        a.getCh(); // a=""   b"i"
        sys_assert(1, b.getCh() == 'i' && a.getCh() == 0 ? 1:0)
    )");
}

TEST(Parser, StringEscapes) {
    execute(R"-(
        using sys { assert }
        s = "\n\t\r\"\\\1090e\\65\!";
        assert(0x0a, s.getCh());
        assert(9, s.getCh());
        assert(0x0d, s.getCh());
        assert('"', s.getCh());
        assert('\', s.getCh());
        assert(0x1090e, s.getCh());
        assert(0x65, s.getCh());
        assert('!', s.getCh());
        assert(0, s.getCh());
    )-");
}

TEST(Parser, SetOps) {
    execute(R"(
        class Cl{
            x = 0xc;
            getAt(i int) int { x }
            setAt(i int, v int) { x := v }
            inc() {
                x += 1
            }
        }
        a = 1;    // a=1   x=0xc
        a *= 10;  // a=10
        a += 4;   // a=14
        c = Cl;
        c.x |= 3;  //     x=0xf(15)
        c[4] /= 3; //     x = 5
        c.inc();   //     x=6
        sys_assert(20, c.x + a)    // 20
    )");
}

TEST(Parser, GetParent) {
    execute(R"(
        using sys{ assert }
        class Cl{
            x = 0;
            inner = ?Cl;
            new(x int) this { this.x := x }
        }
        a = Cl.new(11);
        a.inner := Cl.new(22);
        a.inner?_.inner := Cl.new(33);
        assert(0, sys_getParent(a) && _~Cl ? _.x : 0);
        assert(11, a.inner && sys_getParent(_) && _~Cl ? _.x : 0);
        p = a.inner;
        a.inner := ?Cl;
        assert(0, p
            ? (sys_getParent(_) && _~Cl ? _.x : 0)
            : -1);
        assert(22, p && _.inner && sys_getParent(_) && _~Cl ? _.x : 0)
    )");
}

TEST(Parser, GetParentArray) {
    execute(R"(
        using sys{
            Array,
            Object,
            assert,
            par = getParent,
            getParent
        }
        a = Array(Object);
        a.insertItems(0, 10);
        a[0] := Object;
        assert(a[0] && par(_) && _==a ? 1:0, 1);
        v = a[0];
        a.delete(0, 1);
        assert(v && !par(_) ? 1 : 0, 1);
        a[0] := Object;
        assert(a[0] && par(_) && _==a ? 1:0, 1);
        v := a[0];
        a.setOptAt(0, ?Object);
        assert(v && !getParent(_) ? 1 : 0, 1);
        assert(!getParent(a) ? 1 : 0, 1)
    )");
}

TEST(Parser, Splice) {
    execute(R"(
        using sys{assert}
        class C{ inner = ?C; }
        a = C;
        a.inner := C;
        temp = a.inner;
        a.inner := ?C;
        assert(temp && !sys_getParent(_) ? 1:0, 1);
        a.inner @= temp;
        assert(temp == a.inner ? 1:0, 1)
    )");
}

TEST(Parser, Shared) {
    execute(R"-(
      using sys{assert}
      class Point {
         x = 0;
         y = 0;
         z = *"";
         at(x int, y int) this {
            this.x := x;
            this.y := y
         }
         *sum() int { x+y }
      }
      p = *Point.at(1, 2);
      p2 = p;
      assert(p2.sum(), 3);
      assert(p == p2 ? 1:0, 1)
    )-");
}

TEST(Parser, Consts) {
    execute(R"-(
      using sys{assert}
      const xDefPoint = *Point;
      const xCount = 4;
      const xHello = *"Hello world";

      class Point {
         x = 0;
         y = 0;
      }
      p = @xDefPoint;
      sys_log(xHello);
      assert(xCount, 4)
    )-");
}

TEST(Parser, Multiline) {
    execute(R"-(
      sys_log("..../
         Multiline
         string
      ");
    )-");
}

TEST(Parser, StringInterpolation) {
    execute(R"-(
      using sys {
            String,
            StrBuilder
      }
      class StrBuilder{
            pos = 0;
            put(codePoint int) this {
                size = capacity();
                growStep = 100;
                pos + 5 >= size ?
                    insertItems(size, growStep);
                pos := putChAt(pos, codePoint)
            }
            putStr(s String) this {
                loop !{
                    c = s.getCh();
                    c != 0 ? put(c)
                }
            }
            putInt(v int) this {
                put(v % 10 + '0')
            }
            toStr() @String {
                r = String;
                r.fromBlob(this, 0, pos);
                pos := 0;
                r
            }
      }
      sys_log(`Hi {2*2}!`);
      sys_log("${}/
         Name=${
            1 < 2
                ? "asdf"
                : "zxcv"}
         Age=${2 * 2}
      ");
    )-");
}

TEST(Parser, Generics) {
    execute(R"-(
      using sys { String, log }
      class Pair(X) {
          a = ?X;
          b = ?X;
          set(a X, b X) this {
            this.a := @a;
            this.b := @b;
          }
      }
      p = Pair(String).set("Hello", "World");
      log(p.a : "none");
    )-");
}

TEST(Parser, ReopenGenerics) {
    execute(R"-(
        class sys_WeakArray{
            append(item T) T {
                insertItems(capacity(), 1);
                this[capacity() - 1] := &item;
                item
            }
        }
        a = sys_WeakArray(sys_Object);
        {
          x = sys_Object;
          a.append(x);
          sys_assert(1, a[0] && x == _ ? 1:0);
        };
        sys_assert(1, a[0] ? 0:1);
    )-");
}

TEST(Parser, GenericFromGeneric) {
    execute(R"-(
        using sys { Array, String, Blob }
        class Pair(A, B) {
            a = ?A;
            b = ?B;
        }
        class Map(K, V) {
            +Array(Pair(K, V));
        }
        class Dict(X) {
            +Map(String, X);
        }
        d = Dict(Blob);
        d[0] ? _.b ? _.capacity();
    )-");
}


TEST(Parser, GenericInstAsType) {
    execute(R"-(
        using sys { Array, String, log }
        fn myFn(s Array(String)) {
           s[0] ? log(_)
        }
        myFn({
           a = Array(String);
           a.insertItems(0, 1);
           a[0] := "Aloha";
           a
        })
    )-");
}

TEST(Parser, AsyncInner) {
    execute(R"-(
        class App{
            asyncOp() {
                sys_setMainObject(?sys_Object);
            }
        }
        app = App;
        sys_setMainObject(app);
        app.asyncOp~();
    )-");
}

TEST(Parser, AsyncFfi) {
    execute(R"-(
        fn callbackInvoker(callback &());
        class App{
            onCallback() {
                sys_setMainObject(?sys_Object);
            }
        }
        app = App;
        sys_setMainObject(app);
        callbackInvoker(app.onCallback);
    )-");
}

TEST(Parser, Multithreading) {
    execute(R"-(
        class App{
            worker = sys_Thread(sys_Object).start(sys_Object);
        }
        app = App;
        sys_setMainObject(app);
        sys_log("Started on main thread\n");
        app.worker.root().&workerCode(onEnd &()){
            sys_log("Hello from the worker thread\n");
            onEnd~();
        }~(app.&endEverything(){
            sys_log("Shutdown from the main thread\n");
            sys_setMainObject(?sys_Object);        
        });
    )-");
}

TEST(Parser, FnReturn) {
    execute(R"-(
        fn myFunction() int {
            i = 2;
            loop {
                i -= 1;
                i == 0 ? ^myFunction=42;
                i < 0
            };
            11
        }
        sys_assert(42, myFunction())
    )-");
}

TEST(Parser, Break) {
    execute(R"-(
        fn myFunction() int {
            i = 2;
            r = loop {
                i -= 1;
                i == 0 ? ^r = 42;
            };
            r
        }
        sys_assert(42, myFunction())
    )-");
}

TEST(Parser, Unwind) {
    execute(R"-(
        fn forRange(from int, to int, body(int)) {
            loop !(from < to ? {
                    body(from);
                    from += 1
                })
        }
        x = {
                forRange(0, 3, (i){
                    i == 1 ? ^x=42
                });
                0
            };
        sys_assert(42, x)
    )-");
    /* TODO: move to a separate "internals" doc
        fn forRange(from int, to int, body(int)>>>bool<<<) >>>bool<<< { // for all callables having lambda paramters and for all lambdas (can_x_break) - one optional level added
            loop !(from < to ? {
                    body(from) >>> : ^forRange=false <<<;  // for all calls inside "can_x_break" if it is not proven that all lambdas are not cross-breaking: check_for_null ? ret_null : unwrap
                    from += 1                              // overwise if it is proven that calle is not cross-breaking, add unconditional unwrap
                })
            >>> true <<<  // for all "can_x_break" adds wrap to all normal rets
        }
        x = >>>{  // for all blocks that are cross-break targets (BT): add captured optional var
            v = false; <<<
            forRange(1, 100, (i)>>>bool<<<{                 // for all crossbreaking lambdas (CL), one optional level
                i == 11 ? >>>{ v = 42; ^forRange=false }<<<  // crossbreaks assign to BT var and breaks nullopt from CL
                >>>true<<<             // CL adds wrap to all normal rets
            }) : v ? ^x= _ : ^main; // BT adds to adds to all CHLP calls check for result and BT var
            >>>
            0
        };<<<
        sys_assert(42, x)

        modified are:
            *crossbreaks target blocks: add var
            *can_x_break callables: add +opt to result, and wrap in each normal return
            *cross-breaks: assign to var, return nullopt from the current fn.
            calls to "can_x_break":
                if all possibly activated lambdas don't cross-break: forcefully unwrap result
                else:
                    check result for not null and unwrap
                    for each break in each lambda:
                        check its var and break its block
                    if has lambda params, return from current fn
                    if no breaks and the current callable has no lambda params, error
        attributes:
            blocks*: first name is _x_var
            break: x_var pointer, if null it's not an xbreak
            lambda*: x_targets all outer blocks activated by this lambda breaks (and if lambda calls another lambdas, their x_targets are also added to this list)
            callable_type*: can_x_break - true for all lambdas and (callables having lambda params)
            callsite*: list of lambdas that can be invoked:
               - every lambda (by trace) that can be a callee
               - every lambda (by trace) that can be a parameter

        Go: Robert Griesemer <gri@google.com>
        Carbon: Jason Parachoniak <jparachoniak@google.com>
        cinder
    */
}

}  // namespace
