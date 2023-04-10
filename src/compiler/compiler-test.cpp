#include <unordered_map>
#include "utils/fake-gunit.h"
#include "dom/dom-to-string.h"
#include "compiler/ast.h"
#include "compiler/parser.h"
#include "compiler/name-resolver.h"
#include "compiler/type-checker.h"

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
    ast->platform_exports.insert({ "ag_fn_sys_foreignTestFunction", (void(*)())(foreign_test_function) });
    ast->mk_fn(ast->dom->names()->get("assert"), (void(*)())(ag_assert), new ast::ConstVoid, { ast->tp_int64(), ast->tp_int64() });
    auto start_module_name = ast->dom->names()->get("ak")->get("test");
    unordered_map<own<Name>, string> texts{ {start_module_name, source_text} };
    std::unordered_set<ltm::pin<dom::Name>> modules_in_dep_path;
    parse(ast, start_module_name, modules_in_dep_path, [&](pin<Name> name) {
        return move(texts[name]);
    });
    resolve_names(ast);
    check_types(ast);
    if (dump_all)
        std::cout << std::make_pair(ast.pinned(), ast->dom.pinned()) << "\n";
    foreign_test_function_state = 0;
    generate_and_execute(ast, false, dump_all);
}

TEST(Parser, Ints) {
    execute("assert(7, (2 ^ 2 * 3 + 1) << (2-1) | (2+2) | (3 & (2>>1)))");
}

TEST(Parser, Doubles) {
    execute("assert(3, int(3.14 - 0.2e-4 * 5.0))");
}

TEST(Parser, Block) {
    execute("assert(3, {1+1}+1)");
}

TEST(Parser, Functions) {
    execute(R"-(
      fn plus1(int a) int {
            a + 1
      }
      assert(9, plus1(4) + plus1(3))
    )-");
}

TEST(Parser, LambdaWithParams) {
    execute("assert(40, (a){1+a}(3)*10)");
}

TEST(Parser, PassingLambdaToLambda) {
    execute(R"-( assert(99,
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
    execute("assert(44, {1; 3.14; 44})");
}

TEST(Parser, LocalAssignment) {
    execute("assert(10, {a = 2; a := a + 3; a * 2})");
}

TEST(Parser, MakeAndConsumeOptionals) {
    execute("assert(2, (opt){ opt : 2 } (false ? 44))");
}

TEST(Parser, MaybeChain) {
    execute(R"(assert(3, {
        a = +3.3;  // a: local of type optional(double), having value just(3.3)
        a ? int(_) : 0  // convert optional(double) to optional(int) and extract replacing `none` with 0
    })
    )");
}

TEST(Parser, IntLessThan) {
    execute("assert(3, { a = 2; a < 10 ? 3 : 44 })");
}

TEST(Parser, IntNotEqual) {
    execute("assert(3, { a = 2; a != 10 ? 3 : 44 })");
}

TEST(Parser, Loop) {
    execute(R"(
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
        assert(3, p.y + p.x)
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
        assert(4, p.x + pb.x)
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
        assert(32, p.m())
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
        assert(37, p.m())
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
        assert(201, p.x + p.size.y)
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
        assert(24, p~Widget?_.color : 0)     // cast `p` to `Widget` and return its `color` field on success
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
        assert(47, a + b)
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
        assert(22, r)
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

        assert(35, oldSize * 10 + root.scan(&Node))
    )");
}

TEST(Parser, ForeignFunctionCall) {
    execute(R"(
        fn sys_foreignTestFunction(int x) int;
        sys_foreignTestFunction(4*10);
        assert(42, sys_foreignTestFunction(2))
    )");
}

TEST(Parser, LogicalOr) {
    execute(R"(
        a = 3;
        assert(42, a > 2 || a < 4 ? 42 : 0)
    )");
}

TEST(Parser, LogicalAnd) {
    execute(R"(
        a = 3;
        assert(42, a > 2 && a < 4 ? 42 : 0)
    )");
}

TEST(Parser, Raii) {
    execute(R"(
        class Font {
            ttfHandle = 0;
            setId(int id) {
                ttfHandle != 0 ? sys_foreignTestFunction(-ttfHandle);
                ttfHandle := id;
                id != 0 ? sys_foreignTestFunction(id);
            }
        }
        fn Font_dispose(Font f) {
            f.ttfHandle != 0 ? sys_foreignTestFunction(-f.ttfHandle);
        }
        fn Font_afterCopy(Font f) {
            f.ttfHandle != 0 ? sys_foreignTestFunction(f.ttfHandle);
        }
        fn sys_foreignTestFunction(int x) int;

        {
            sys_foreignTestFunction(2);
            fa = Font;
            fa.setId(42);   // sys_foreignTestFunction_state= 2+42
            fb = @fa;       // sys_foreignTestFunction_state= 2+42+42
        };                  // sys_foreignTestFunction_state= 2+42+42-42-42 = 2
        assert(2, sys_foreignTestFunction(0))
    )");
}

TEST(Parser, BlobsAndIndexes) {
    execute(R"(
        b = sys_Blob;
        sys_Container_insert(b, 0, 3);
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
        sys_Container_insert(a, 0, 10);
        a[0] := Node;
        a[1] := Node;
        a[1] && _~Node ? {
            _.x := 42;
            _.y := 33;
        };
        c = @a;
        sys_Array_delete(c, 0, 1);
        assert(42, c[0] && _~Node ? _.x : -1)
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
        sys_Container_insert(a, 0, 10);
        a[0] := &n;
        a[1] := &n;
        c = @a;
        sys_WeakArray_delete(c, 0, 1);
        assert(1, c[0]&&_==n ? 1:0)
    )");
}

TEST(Parser, OpenClasses) {
    execute(R"(
        class sys_Array {                              // Any methods can be added to existing classes.
            insert(int at, int count) {
                sys_Container_insert(this, at, count);
            }
            delete(int at, int count) {
                sys_Array_delete(this, at, count);
            }
        }
        class Node {
            x = 42;
            y = 0;
        }
        a = sys_Array;
        a.insert(0, 10);
        a[1] := Node;
        c = @a;
        c.delete(0, 1);
        assert(42, c[0]&&_~Node?_.x : -1)
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
        sys_Container_insert(a, 0, 1);
        a[0] := nodeAt(42, 33);  // no copy here!
        assert(42, a[0]&&_~Node?_.x : -1)
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
        assert(5, b.n.x + b.n.flags)         // 5
    )");
}

TEST(Parser, TypedArrays) {
    execute(R"(
        class sys_Array {
            add(()@sys_Object n) {               // `add` accepts lambda and calss it to get @object placed exactly where, when and how much objects needed.
                at = sys_Container_size(this);
                sys_Container_insert(this, at, 1);
                sys_Array_setAt(this, at, n());
            }
            size() int { sys_Container_size(this) }
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
                sys_Array_getAt(this, i) && _~Node ? _ : def  // get element, check it for out of bounds or unitialized, cast it to Node and check, on success return it otherwise return default.
            }
        }
        a = NodeArray;
        a.add((){ Node.xy(2, 3) });
        a.add((){ Node.xy(20, 30) });
        assert(34, a[0].x + a[1].y + a.size())
    )");
}

TEST(Parser, StringOperations) {
    execute(R"(
        class sys_String{
            get() int { sys_String_getCh(this) }
            length() int {
                r = 0;
                loop {
                    c = sys_String_getCh(this);
                    c != 0 ? r := r + 1;
                    c == 0 ? r
                }
            }
        }
        class OString {
            +sys_Blob;
            pos = 0;
            put(int codePoint) this {
                size = sys_Container_size(this);
                growStep = 100;
                pos + 5 >= size ?
                    sys_Container_insert(this, size, growStep);
                pos := sys_Blob_putCh(this, pos, codePoint)
            }
            append(sys_String s) this {
                loop{
                    c = sys_String_getCh(s);
                    c != 0 ? put(c);
                    c == 0
                }
            }
            str() @sys_String {
                r = sys_String;
                sys_String_fromBlob(r, this, 0, pos);
                pos := 0;
                r
            }
        }
        a = OString.put('<').append("Hello there").put('>').str();
        assert(13, a.length())
    )");
}

TEST(Parser, LiteralStrings) {
    execute(R"(
        a = "Hi";
        sys_String_getCh(a); // a="i"
        b = @a;
        sys_String_getCh(a); // a=""   b"i"
        assert(1, sys_String_getCh(b) == 'i' && sys_String_getCh(a) == 0 ? 1:0)
    )");
}

TEST(Parser, StringEscapes) {
    execute(R"-(
        s = "\n\t\r\"\\\1090e\\65\!";
        assert(0x0a, sys_String_getCh(s));
        assert(9, sys_String_getCh(s));
        assert(0x0d, sys_String_getCh(s));
        assert('"', sys_String_getCh(s));
        assert('\', sys_String_getCh(s));
        assert(0x1090e, sys_String_getCh(s));
        assert(0x65, sys_String_getCh(s));
        assert('!', sys_String_getCh(s));
        assert(0, sys_String_getCh(s));
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
        assert(20, c.x + a)    // 20
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
        assert(53, f(c.m));
        assert(31, f(c.&diff(int i) int { i - x }))
    )");
}

TEST(Parser, GetParent) {
    execute(R"(
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
        a = sys_Array;
        sys_Container_insert(a, 0, 10);
        a[0] := sys_Object;
        assert(a[0] && sys_getParent(_) && _==a ? 1:0, 1);
        v = a[0];
        sys_Array_delete(a, 0, 1);
        assert(v && !sys_getParent(_) ? 1 : 0, 1);
        a[0] := sys_Object;
        assert(a[0] && sys_getParent(_) && _==a ? 1:0, 1);
        v := a[0];
        a[0] := ?sys_Object;
        assert(v && !sys_getParent(_) ? 1 : 0, 1);
        assert(!sys_getParent(a) ? 1 : 0, 1)
    )");
}

TEST(Parser, Splice) {
    execute(R"(
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
