#include <unordered_map>
#include "utils/fake-gunit.h"
#include "dom/dom-to-string.h"
#include "compiler/ast.h"
#include "compiler/parser.h"
#include "compiler/name-resolver.h"
#include "compiler/type-checker.h"
#include "compiler/const-capture-pass.h"

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
void ag_assert(int64_t expected, int64_t actual) {
    ASSERT_EQ(expected, actual);
}

void execute(const char* source_text, bool dump_all = false) {
    ast::initialize();
    auto ast = own<Ast>::make();
    ast->platform_exports.insert({ "ag_fn_akTest_foreignTestFunction", (void(*)())(foreign_test_function) });
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
      fn plus1(int a) int {
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
      using sys { assert; }
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
          move(int x, int y);
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
            move(int dx, int dy) {
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
          moveTo(int x, int y);
        }
        interface Sizeable {
          resize(int width, int height);
        }
        class Point {
          x = 0;
          y = 0;
        }
        class Widget {
          +Point;
          +Movable {
            moveTo(int x, int y) {
              this.x := x;
              this.y := y;
            }
          }
          +Sizeable {
            resize(int width, int height) {
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
          moveTo(int x, int y);
        }
        class Point {
          +Movable {
            moveTo(int x, int y) {
              this.x := x;
              this.y := y;
            }
          }
          x = 0;
          y = 0;
        }
        class Widget {
          +Movable {
            moveTo(int x, int y) {
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

          scan(&Node expectedParent) int {
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
        fn foreignTestFunction(int x) int;
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
            setId(int id) {
                ttfHandle != 0 ? foreignTestFunction(-ttfHandle);
                ttfHandle := id;
                id != 0 ? foreignTestFunction(id);
            }
        }
        fn disposeFont(Font f) {
            f.ttfHandle != 0 ? foreignTestFunction(-f.ttfHandle);
        }
        fn afterCopyFont(Font f) {
            f.ttfHandle != 0 ? foreignTestFunction(f.ttfHandle);
        }
        fn foreignTestFunction(int x) int;

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
            Blob;
            insertItems;
            assert;
        }
        class sys_Blob {
            getAt(int i) int { sys_get64At(this, i) }
            setAt(int i, int v) { sys_set64At(this, i, v) }
        }
        b = Blob;
        insertItems(b, 0, 3);
        b[1] := 42;
        c = @b;
        assert(42, c[1])
    )");
}

TEST(Parser, Arrays) {
    execute(R"(
        class Node {
            x = 1;
            y = 0;
        }
        a = sys_Array;
        sys_insertItems(a, 0, 10);
        a[0] := Node;
        a[1] := Node;
        a[1] && _~Node ? {
            _.x := 42;
            _.y := 33;
        };
        c = @a;
        sys_deleteItems(c, 0, 1);
        sys_assert(42, c[0] && _~Node ? _.x : -1)
    )");
}

TEST(Parser, WeakArrays) {
    execute(R"(
        class Node {
            x = 1;
            y = 0;
        }
        n = Node;
        a = sys_WeakArray;
        sys_insertItems(a, 0, 10);
        a[0] := &n;
        a[1] := &n;
        c = @a;
        sys_deleteWeaks(c, 0, 1);
        sys_assert(1, c[0] && _==n ? 1:0)
    )");
}

TEST(Parser, RetOwnPtr) {
    execute(R"(
        class Node {
            x = 1;
            y = 0;
        }
        fn nodeAt(int x, int y) @Node {  // Callables can return newly created/copied object
            r = Node;
            r.x := x;
            r.y := y;
            r
        }
        a = sys_Array;
        sys_insertItems(a, 0, 1);
        a[0] := nodeAt(42, 33);  // no copy here!
        sys_assert(42, a[0] && _~Node ? _.x : -1)
    )");
}

TEST(Parser, InitializerMethods) {
    execute(R"(
        class Node {
            x = 1;
            y = 0;
            flags = 0;
            initPos(int x, int y) this {  // Functions marked with this when called on @pointer return @pointer, and with correct derived type!
                this.x := x;
                this.y := y;
            }
            transpose() this {
                t = this.x;
                this.x := this.y;
                this.y := t;
            }
            addFlags(int f) this {
                this.flags := this.flags | f;
            }
            removeFlags(int f) this {
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

TEST(Parser, TypedArrays) {
    execute(R"(
        class sys_Array {
            add(()@sys_Object n) {               // `add` accepts lambda and calss it to get @object placed exactly where, when and how much objects needed.
                at = sys_getSize(this);
                sys_insertItems(this, at, 1);
                sys_setAtArray(this, at, n());
            }
            size() int { sys_getSize(this) }
        }
        class Node {
            x = 1;
            y = 0;
            xy(int x, int y) this {
                this.x:=x;
                this.y:=y;
            }
        }
        class NodeArray {
            +sys_Array;
            def = Node.xy(0, 0);   // `def` - default object to be returned on errors
            getAt(int i) Node {
                sys_getAtArray(this, i) && _~Node ? _ : def  // get element, check it for out of bounds or unitialized, cast it to Node and check, on success return it otherwise return default.
            }
        }
        a = NodeArray;
        a.add((){ Node.xy(2, 3) });
        a.add(Node.xy(20, 30));                    // @T to ()@T conversion
        sys_assert(34, a[0].x + a[1].y + a.size())
    )");
}

TEST(Parser, StringOperations) {
    execute(R"(
        class sys_String{
            get() int { sys_getCh(this) }
            length() int {
                r = 0;
                loop {
                    c = sys_getCh(this);
                    c != 0 ? r := r + 1;
                    c == 0 ? r
                }
            }
        }
        class OString {
            +sys_Blob;
            pos = 0;
            put(int codePoint) this {
                size = sys_getSize(this);
                growStep = 100;
                pos + 5 >= size ?
                    sys_insertItems(this, size, growStep);
                pos := sys_putCh(this, pos, codePoint)
            }
            append(sys_String s) this {
                loop{
                    c = sys_getCh(s);
                    c != 0 ? put(c);
                    c == 0
                }
            }
            str() @sys_String {
                r = sys_String;
                sys_stringFromBlob(r, this, 0, pos);
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
        sys_getCh(a); // a="i"
        b = @a;
        sys_getCh(a); // a=""   b"i"
        sys_assert(1, sys_getCh(b) == 'i' && sys_getCh(a) == 0 ? 1:0)
    )");
}

TEST(Parser, StringEscapes) {
    execute(R"-(
        using sys {
            assert;
            getCh;
        }
        s = "\n\t\r\"\\\1090e\\65\!";
        assert(0x0a, getCh(s));
        assert(9, getCh(s));
        assert(0x0d, getCh(s));
        assert('"', getCh(s));
        assert('\', getCh(s));
        assert(0x1090e, getCh(s));
        assert(0x65, getCh(s));
        assert('!', getCh(s));
        assert(0, getCh(s));
    )-");
}

TEST(Parser, SetOps) {
    execute(R"(
        class Cl{
            x = 0xc;
            getAt(int i) int { x }
            setAt(int i, int v) { x := v }
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

TEST(Parser, Delegates) {
    execute(R"(
        class Cl{
            x = 11;
            m(int i) int { x + i }
        }
        fn f(&(int)int handler) int {
            handler(42) : 0
        }
        c = Cl;
        sys_assert(53, f(c.m));
        sys_assert(31, f(c.&diff(int i) int { i - x }))
    )");
}

TEST(Parser, GetParent) {
    execute(R"(
        using sys{ assert; }
        class Cl{
            x = 0;
            inner = ?Cl;
            new(int x) this { this.x := x }
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
            Array;
            Object;
            assert;
            ins = insertItems;
            del = deleteItems;
            par = getParent;
            getParent;
        }
        a = Array;
        ins(a, 0, 10);
        a[0] := Object;
        assert(a[0] && par(_) && _==a ? 1:0, 1);
        v = a[0];
        del(a, 0, 1);
        assert(v && !par(_) ? 1 : 0, 1);
        a[0] := Object;
        assert(a[0] && par(_) && _==a ? 1:0, 1);
        v := a[0];
        sys_setOptAt(a, 0, ?Object);
        assert(v && !getParent(_) ? 1 : 0, 1);
        assert(!getParent(a) ? 1 : 0, 1)
    )");
}

TEST(Parser, Splice) {
    execute(R"(
        using sys{assert;}
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
      using sys{assert;}
      class Point {
         x = 0;
         y = 0;
         z = *"";
         at(int x, int y) this {
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

}  // namespace
