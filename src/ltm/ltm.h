/*
Copyright 2018 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef _AK_LTM_H_
#define _AK_LTM_H_

#include <cstdint>
#include <cstdlib>
#include <utility>
#include <type_traits>
#include <ostream>

namespace ltm {

class Object;
class WeakBlock;

template <typename T> class own;
template <typename T> class weak;
template <typename T> class pin;
template <typename T> class iweak;
template <typename T> class ipin;

class Object {
	friend class WeakBlock;
	template <typename BASE> friend class Proxy;
	template <typename T> friend class own;
	template <typename T> friend class weak;
	template <typename T> friend class pin;
	template <typename T> friend class iweak;
	template <typename T> friend class ipin;
	friend struct LtmTester;
public:
	template <typename FROM, typename TO> static void copy(FROM begin, FROM end, TO dst) {
		copy_transaction transaction;
		while (begin != end)
			*dst++ = *begin++;
	}

	template <typename VECTOR> static void copy(
		VECTOR& container,
		std::size_t begin,
		std::size_t end,
		std::size_t dst)
	{
		copy_transaction transaction;
		while (begin != end)
			container.insert(container.begin() + dst++, container[begin++]);
	}

protected:
	Object() noexcept;
	Object(const Object& src) noexcept;
	virtual void internal_dispose() noexcept;
	virtual ~Object() noexcept;

	// WeakBlock and Proxy objects can override it
	virtual Object* get_weak();

	// Implemented in all ltm::objects as dst = new ThisClass(*this);
	virtual void copy_to(Object*& dst) = 0;

	// WeakBlock and Proxy objects can override it
	virtual Object* get_target() noexcept;

	// Used by application code to make owning ptrs act as pins.
	void make_shared() noexcept;

	enum {
		// 1 - it's counter,
		// 0 - it's weak_block ptr, and the real counter located in
		// weak_block->org_counter
		WEAKLESS = intptr_t(1),

		// 0 - assignment to a owning ptr just sets this bit,
		// 1 - assignment to a owning makes a full copy
		OWNED = intptr_t(2),

		// Copy acts as retain
		SHARED = intptr_t(4),

		// If its weak block contains additional atomic counters.
		ATOMIC = intptr_t(8),

		// Number of owning ptrs (if shared) + pin-ptrs pointing here,
		// if 0 - deleted
		COUNTER_STEP = intptr_t(16),
  };

 private:
	union {
		intptr_t counter;
		WeakBlock* weak_block;
	};

	// Used by owning poiners.
	static void outer_copy_to(Object* src, Object*& dst);
	static void force_copy_to(Object* src, Object*& dst);
	void finalize_copy(Object*& dst);

	// Used by weak pointers
	static Object* get_weak(Object* me);
	static Object* get_target(Object* me) noexcept;

	// Used by pin-ptrs
	static Object* retain(Object* me) noexcept;

	// Used by all ptrs: owning, weak and pin.
	static void release(Object* me) noexcept;

	void operator=(const Object&) = delete;

	struct copy_transaction {
		copy_transaction();
		~copy_transaction();
	};
};

template <typename BASE>
class Proxy : public BASE {
protected:
	void copy_to(Object*&) override {
		abort();  // call make_shared or implement copy_to
	}
	Object* get_weak() override {
		this->counter += Object::COUNTER_STEP;
		return this;
	}
	Object* get_target() noexcept override { return this; }
};

class WeakBlock : public Object {
	friend class Object;
	friend struct Object::copy_transaction;
	friend struct LtmTester;
	template <typename T> friend class own;
	template <typename T> friend class weak;
	template <typename T> friend class pin;
	template <typename T> friend class iweak;
	template <typename T> friend class ipin;

	Object* target;
	intptr_t org_counter;
	WeakBlock(Object* target, intptr_t org_counter) noexcept;
	WeakBlock(const WeakBlock&) = delete;

protected:
	void copy_to(Object*& dst) override;
	Object* get_weak() override;
	Object* get_target() noexcept override;
};

// Owning pointer
template <typename T>
class own {
	friend struct LtmTester;
	template <typename U> friend class pin;
	template <typename U> friend class weak;
	template <typename U> friend class ipin;
	template <typename U> friend class iweak;

	mutable Object* target;

public:
	typedef T wrapped;
	own() noexcept : target() {}
	own(std::nullptr_t) noexcept : target() {}
	own(const own& src) { Object::outer_copy_to(src.target, target); }
	own(own&& src) noexcept : target(src.target) { src.target = nullptr; }

	template <
		typename U,
		typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
	own(U* src) : target() {
		Object::outer_copy_to(src, target);
	}

	~own() noexcept { Object::release(target); }
	T* operator->() const noexcept { return static_cast<T*>(target); }
	T& operator*() const noexcept { return *static_cast<T*>(target); }
	operator bool() const noexcept { return target != nullptr; }
	operator void*() const noexcept { return target; }

	own<T>& operator=(const own& src) {
		if (&src != this) {
			Object* temp;
			Object::outer_copy_to(src.target, temp);
			Object::release(target);
			target = temp;
		}
		return *this;
	}

	own<T>& operator=(own&& src) noexcept {
		Object::release(target);
		target = src.target;
		src.target = nullptr;
		return *this;
	}

	template <
		typename U,
		typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
	own<T>& operator=(U&& src) {
		own<T> t(std::forward<U>(src));
		std::swap(t.target, target);
		return *this;
	}

	template <
		typename U = T,
		typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
	own<U> distinct_copy(const pin<U>& src) const {
		own<U> r;
		Object::force_copy_to(src.target, r.target);
		return r;
	}

	template <
		typename BASE,
		typename = typename std::enable_if<std::is_convertible<T*, BASE*>::value>::type>
	operator own<BASE>&() noexcept {
		return *reinterpret_cast<own<BASE>*>(this);
	}

	template <
		typename SUB,
		typename = typename std::enable_if<std::is_convertible<SUB*, T*>::value>::type>
	const own<SUB>& cast() const noexcept {
		return *reinterpret_cast<const own<SUB>*>(this);
	}

	template <
		typename SUB,
		typename = typename std::enable_if<std::is_convertible<SUB*, T*>::value>::type>
	own<SUB>& cast() noexcept {
		return *reinterpret_cast<own<SUB>*>(this);
	}

	template <
		typename C = T,
		typename = typename std::enable_if<std::is_convertible<C*, T*>::value>::type,
		typename... P>
	void set(P&&... p) {
		Object* temp;
		Object::outer_copy_to(new C(std::forward<P>(p)...), temp);
		Object::release(target);
		target = temp;
	}

	template <
		typename C = T,
		typename = typename std::enable_if<std::is_convertible<C*, T*>::value>::type,
		typename... P>
	static own<T> make(P&&... p) {
		return own<T>(new C(std::forward<P>(p)...));
	}

	template <typename U = T>
	weak<U> weaked() const {
		return ::ltm::weak<U>(*this);
	}

	template <typename U = T>
	pin<U> pinned() const {
		return ::ltm::pin<U>(*this);
	}
};

// Temporay pointer
template <typename T>
class pin {
	friend struct LtmTester;
	template <typename U> friend class own;
	template <typename U> friend class weak;
	template <typename U> friend class iweak;

	mutable Object* target;

public:
	typedef T wrapped;
	pin() noexcept : target() {}
	pin(std::nullptr_t) noexcept : target() {}
	pin(const pin& src) noexcept : target(Object::retain(src.target)) {}
	pin(pin&& src) noexcept : target(src.target) { src.target = nullptr; }

	template <
		typename U,
		typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
	pin(const own<U>& src) noexcept : target(Object::retain(src.target)) {}

	template <
		typename U,
		typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
	pin(U* src) noexcept : target(Object::retain(src)) {}

	T* operator->() const noexcept { return static_cast<T*>(target); }
	operator bool() const noexcept { return target != nullptr; }
	operator void*() const noexcept { return target; }
	T& operator*() const noexcept { return *static_cast<T*>(target); }
	~pin() noexcept { Object::release(target); }
	bool has_weak() { return target && (target->counter & Object::WEAKLESS) == 0; }

	template <
		typename BASE,
		typename = typename std::enable_if<std::is_convertible<T*, BASE*>::value>::type>
	operator own<BASE>() const {
		return own<BASE>(static_cast<BASE*>(target));
	}

	pin<T>& operator=(const pin& src) noexcept {
		if (&src != this) {
		Object::retain(src.target);
		Object::release(target);
		target = src.target;
		}
		return *this;
	}

	pin<T>& operator=(pin&& src) noexcept {
		Object::release(target);
		target = src.target;
		src.target = nullptr;
		return *this;
	}

	template <
		typename U,
		typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
	pin<T>& operator=(U&& src) noexcept {
		pin<T> t{std::forward<U>(src)};
		std::swap(t.target, target);
		return *this;
	}

	template <
		typename BASE,
		typename = typename std::enable_if<std::is_convertible<T*, BASE*>::value>::type>
	operator const pin<BASE>&() const noexcept {
		return *reinterpret_cast<const pin<BASE>*>(this);
	}

	template <
		typename BASE,
		typename = typename std::enable_if<std::is_convertible<T*, BASE*>::value>::type>
	operator pin<BASE>&() noexcept {
		return *reinterpret_cast<pin<BASE>*>(this);
	}

	template <typename SUB>
	pin<SUB>& cast() noexcept {
		return *reinterpret_cast<pin<SUB>*>(this);
	}

	template <typename SUB>
	const pin<SUB>& cast() const noexcept {
		return *reinterpret_cast<const pin<SUB>*>(this);
	}

	template <
		typename SUB = T,
		typename = typename std::enable_if<std::is_convertible<SUB*, T*>::value>::type>
	SUB* get() noexcept {
		return static_cast<SUB*>(target);
	}

	template <
		typename SUB = T,
		typename = typename std::enable_if<std::is_convertible<SUB*, T*>::value>::type>
	const SUB* get() const noexcept {
		return static_cast<const SUB*>(target);
	}

	template <
		typename C = T,
		typename = typename std::enable_if<std::is_convertible<C*, T*>::value>::type,
		typename... P>
	void set(P&&... p) {
		Object* temp = new C(std::forward<P>(p)...);
		Object::release(target);
		target = temp;
		Object::retain(temp);
	}

	template <
		typename C = T,
		typename = typename std::enable_if<std::is_convertible<C*, T*>::value>::type,
		typename... P>
	static pin<T> make(P&&... p) {
		return pin<T>(new C(std::forward<P>(p)...));
	}

	template <
		typename U = T,
		typename = typename std::enable_if<std::is_convertible<T*, U*>::value>::type>
	weak<U> weaked() const {
		return ::ltm::weak<U>(*this);
	}

	template <
		typename U = T,
		typename = typename std::enable_if<std::is_convertible<T*, U*>::value>::type>
	own<U> owned() const {
		return ::ltm::own<U>(*this);
	}

	template <typename X> pin<T>&& mark(X*& ptr) && {
		ptr = static_cast<X*>(target);
		return std::move(*this);
	}
};

// Weak pointer
template <typename T>
class weak {
	friend struct LtmTester;
	template <typename U> friend class own;
	template <typename U> friend class pin;
	template <typename U> friend class ipin;

	mutable Object* target;

public:
	typedef T wrapped;
	weak() noexcept : target() {}
	weak(std::nullptr_t) noexcept : target() {}
	weak(const weak& src) noexcept { Object::outer_copy_to(src.target, target); }
	weak(weak&& src) noexcept : target(src.target) { src.target = nullptr; }

	template <
		typename U,
		typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
	weak(U* src) : target(Object::get_weak(src)) {}

	template <
		typename U,
		typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
	weak(const own<U>& src) : target(Object::get_weak(src.target)) {}

	template <
		typename U,
		typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
	weak(const pin<U>& src) : target(Object::get_weak(src.target)) {}

	~weak() noexcept { Object::release(target); }

	template <
		typename BASE,
		typename = typename std::enable_if<std::is_convertible<T*, BASE*>::value>::type>
	operator own<BASE>() const {
		return own<BASE>(Object::get_target(target));
	}

	template <
		typename BASE,
		typename = typename std::enable_if<std::is_convertible<T*, BASE*>::value>::type>
	operator pin<BASE>() const noexcept {
		return pin<BASE>(static_cast<BASE*>(Object::get_target(target)));
	}

	pin<T> operator->() const noexcept {
		return pin<T>(static_cast<T*>(Object::get_target(target)));
	}

	operator bool() const noexcept {
		return Object::get_target(target) != nullptr;
	}

	operator void*() const noexcept { return Object::get_target(target); }

	weak<T>& operator=(const weak& src) noexcept {
		if (&src != this) {
			Object* temp = Object::get_weak(src.target);
			Object::release(target);
			target = temp;
		}
		return *this;
	}

	weak<T>& operator=(weak&& src) noexcept {
		Object::release(target);
		target = src.target;
		src.target = nullptr;
		return *this;
	}

	template <
		typename U,
		typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
	weak<T>& operator=(U&& src) noexcept {
		weak<T> t(std::forward<U>(src));
		std::swap(t.target, target);
		return *this;
	}

	template <
		typename BASE,
		typename = typename std::enable_if<std::is_convertible<T*, BASE*>::value>::type>
	operator weak<BASE>&() noexcept {
		return *reinterpret_cast<weak<BASE>*>(this);
	}

	template <
		typename SUB,
		typename = typename std::enable_if<std::is_convertible<SUB*, T*>::value>::type>
	weak<SUB>& cast() noexcept {
		return *reinterpret_cast<weak<SUB>*>(this);
	}

	template <
		typename U = T,
		typename = typename std::enable_if<std::is_convertible<T*, U*>::value>::type>
	own<U> owned() const {
		return ::ltm::own<U>(*this);
	}

	template <
		typename U = T,
		typename = typename std::enable_if<std::is_convertible<T*, U*>::value>::type>
	pin<U> pinned() const {
		return ::ltm::pin<U>(*this);
	}
};

template <
	typename A,
	typename B,
	typename = typename std::enable_if<std::is_convertible<A, void*>::value>::type,
	typename = typename std::enable_if<std::is_convertible<B, void*>::value>::type>
bool operator==(const A& a, const B& b) {
	return static_cast<void*>(a) == static_cast<void*>(b);
}

template <
	typename A,
	typename B,
	typename = typename std::enable_if<std::is_convertible<A, void*>::value>::type,
	typename = typename std::enable_if<std::is_convertible<B, void*>::value>::type>
bool operator!=(const A& a, const B& b) {
	return static_cast<void*>(a) != static_cast<void*>(b);
}

template <typename INTERFACE>
class iweak {
	friend struct LtmTester;
	template <typename INTERFACE1> friend class ipin;

	weak<Object> impl;
	std::ptrdiff_t offset;

public:
	typedef INTERFACE wrapped;

	template <typename IMPL>
	iweak(weak<IMPL> impl)
		: impl(std::move(impl))
	{
		auto a = reinterpret_cast<IMPL*>(this);
		auto b = static_cast<INTERFACE*>(a);
		offset = reinterpret_cast<char*>(b) - reinterpret_cast<char*>(a);
	}

	ipin<INTERFACE> pinned() { return ipin<INTERFACE>(*this); }
};

template <typename INTERFACE>
class ipin {
	friend struct LtmTester;
	pin<Object> holder;
	INTERFACE* impl;

public:
	typedef INTERFACE wrapped;

	ipin(const iweak<INTERFACE>& weak)
		: holder(weak.impl)
		, impl(reinterpret_cast<INTERFACE*>(
			reinterpret_cast<char*>(holder.operator->()) + weak.offset)) {}

	operator bool() { return holder; }
	INTERFACE* operator->() { return impl; }
};

template <typename C, typename T>
void mc(C& container, std::initializer_list<T> v) {
	container.reserve(v.size());
	for (auto& i : v)
		container.push_back(i);
}

} // namespace ltm

namespace std {

template <typename T>
struct hash<ltm::weak<T>> {
	typedef ltm::weak<T> argument_type;
	typedef std::size_t result_type;
	result_type operator()(argument_type const& val) const noexcept {
		return std::hash<void*>{}(val);
	}
};

template <typename T>
struct hash<ltm::own<T>> {
	typedef ltm::own<T> argument_type;
	typedef std::size_t result_type;
	result_type operator()(argument_type const& val) const noexcept {
		return std::hash<void*>{}(val);
	}
};

template <typename T>
struct hash<ltm::pin<T>> {
	typedef ltm::pin<T> argument_type;
	typedef std::size_t result_type;
	result_type operator()(argument_type const& val) const noexcept {
		return std::hash<void*>{}(val);
	}
};

template <typename T>
ostream& operator<< (ostream& dst, const ltm::pin<T>& p) {
	return dst << p.operator->();
}

template <typename T>
ostream& operator<< (ostream& dst, const ltm::own<T>& p) {
	return dst << p.operator->();
}

template <typename T>
ostream& operator<< (ostream& dst, const ltm::weak<T>& p) {
	return dst << p.operator->();
}

} // namespace std

#define LTM_COPYABLE(CLASS) \
	void copy_to(Object*& d) override { d = new CLASS(*this); }

#endif  // _AK_LTM_H_
