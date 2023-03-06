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

#include "ltm/ltm.h"

#ifdef TESTS
#include <cassert>
#endif // TESTS

namespace ltm {

namespace {
thread_local Object *copy_head = nullptr;
thread_local uintptr_t copy_depth = 0;

// These tags are used during copy operation.
// They mark three types of pointers.
// 1. Object pointing to its WeakBlock
//    (this link needs no altering, so tag == 0)
// 2. WeakBlock pointing to a next object or first self weak_ptr
//    (normally it points to own Object)
// 3. Weak_ptr value (normally points to WeakBlock,
//    But if tagged points to a next WeakPtr or next Object.
enum class Tag : intptr_t {
	WEAK_BLOCK = 0,
	OBJECT = 1,
	WEAK = 2,
};

template <typename T> T *tag_ptr(void *ptr, Tag tag) noexcept {
	return reinterpret_cast<T *>(reinterpret_cast<intptr_t>(ptr) | static_cast<intptr_t>(tag));
}

template <typename T> T *untag_ptr(void *ptr) noexcept {
	return reinterpret_cast<T *>(reinterpret_cast<intptr_t>(ptr) & ~3);
}

Tag get_ptr_tag(void *ptr) noexcept {
	return static_cast<Tag>(reinterpret_cast<intptr_t>(ptr) & 3);
}
} // namespace

Object::Object() noexcept { counter = WEAKLESS; }

Object::Object(const Object &) noexcept {
  counter = COUNTER_STEP | OWNED | WEAKLESS;
}

Object::~Object() noexcept {
	if ((counter & WEAKLESS) == 0) {
		weak_block->target = nullptr;
		release(weak_block);
	}
}

/*static*/
Object *Object::get_weak(Object *me) { return me ? me->get_weak() : nullptr; }

/*static*/
Object *Object::get_target(Object *me) noexcept {
  return me ? me->get_target() : nullptr;
}

/*static*/
Object *Object::retain(Object *me) noexcept {
	if (me)
		(me->counter & WEAKLESS ? me->counter : me->weak_block->org_counter) += COUNTER_STEP;
	return me;
}

/*static*/
void Object::release(Object *me) noexcept {
	if (!me)
		return;
	intptr_t &counter = me->counter & WEAKLESS ? me->counter : me->weak_block->org_counter;
	if ((counter -= COUNTER_STEP) < COUNTER_STEP)
		me->internal_dispose();
}

void Object::internal_dispose() noexcept { delete this; }

/* static */
void Object::outer_copy_to(Object *src, Object *&dst) {
	if (!src) {
		dst = nullptr;
		return;
	}
	intptr_t &counter = src->counter & WEAKLESS ? src->counter : src->weak_block->org_counter;
	if ((counter & OWNED) == 0) {
		counter += OWNED + COUNTER_STEP;
		dst = src;
		return;
	}
	if (counter & SHARED) {
		counter += COUNTER_STEP;
		dst = src;
		return;
	}
	src->finalize_copy(dst);
}

/* static */
void Object::force_copy_to(Object *src, Object *&dst) {
	if (!src)
		dst = nullptr;
	else {
		src->finalize_copy(dst);
		(dst->counter & WEAKLESS ? dst->counter : dst->weak_block->org_counter) &= ~SHARED;
	}
}

Object::copy_transaction::copy_transaction() { copy_depth++; }

Object::copy_transaction::~copy_transaction() {
	if (--copy_depth == 0) {
		Object *c = nullptr;
		WeakBlock *wb = nullptr;
		for (Object *i = copy_head; i;) {
			switch (get_ptr_tag(i)) {
			case Tag::OBJECT:
				if (c)
					c->counter = Object::COUNTER_STEP | Object::OWNED | Object::WEAKLESS;
				c = untag_ptr<Object>(i);
				i = c->weak_block;
				break;
			case Tag::WEAK_BLOCK:
				wb = untag_ptr<WeakBlock>(i);
				i = wb->target;
				wb->target = c;
				c = nullptr;
				break;
			case Tag::WEAK: {
				WeakBlock **w = untag_ptr<WeakBlock *>(i);
				i = *w;
				*w = wb;
				wb->counter += Object::COUNTER_STEP;
				} break;
			}
		}
		if (c)
			c->counter = Object::COUNTER_STEP | Object::OWNED | Object::WEAKLESS;
		copy_head = nullptr;
	}
}

void Object::finalize_copy(Object *&dst) {
	copy_transaction transaction;
	copy_to(dst);
	if ((counter & WEAKLESS) == 0) { // has weak block
		if (weak_block->target == this) { // no weak copied yet
			weak_block->target = tag_ptr<WeakBlock>(dst, Tag::OBJECT);
			dst->weak_block = static_cast<WeakBlock *>(copy_head);
			copy_head = tag_ptr<Object>(this, Tag::OBJECT);
		} else {
			dst->weak_block = new WeakBlock(dst, COUNTER_STEP | OWNED);
			while (get_ptr_tag(weak_block->target) == Tag::WEAK) {
				WeakBlock *&w = *untag_ptr<WeakBlock *>(weak_block->target);
				weak_block->target = w;
				w = dst->weak_block;
				retain(dst->weak_block);
			}
			dst->weak_block->target = weak_block->target;
			weak_block->target = tag_ptr<WeakBlock>(dst, Tag::OBJECT);
		}
	}
}

Object *Object::get_weak() {
	if (counter & WEAKLESS)
		weak_block = new WeakBlock(this, counter & ~WEAKLESS);
	retain(weak_block);
	return weak_block;
}

Object *Object::get_target() noexcept { return this; }

void Object::make_shared() noexcept {
	(counter & WEAKLESS ? counter : weak_block->org_counter) |= SHARED;
}

WeakBlock::WeakBlock(Object *target, intptr_t org_counter) noexcept
	: target(target)
	, org_counter(org_counter)
{
	counter = OWNED | COUNTER_STEP | WEAKLESS; // not shared to force the copy_to invocation
}

Object *WeakBlock::get_weak() {
	counter += COUNTER_STEP;
	return this;
}

Object *WeakBlock::get_target() noexcept { return target; }

void WeakBlock::copy_to(Object *&dst) {
	if (!target)
		dst = nullptr;
	else if (copy_depth == 1 && (org_counter & SHARED)) {
		retain(this);
		dst = this;
	} else {
		switch (get_ptr_tag(target)) {
		case Tag::WEAK_BLOCK: // tagWB == 0, so it is an uncopied object
			dst = copy_head;
			copy_head = tag_ptr<Object>(target, Tag::OBJECT);
			target = tag_ptr<Object>(&dst, Tag::WEAK);
			break;
		case Tag::WEAK: // already accessed by weak in this copy
			dst = target;
			target = tag_ptr<Object>(&dst, Tag::WEAK);
			break;
		case Tag::OBJECT: { // already copied
			Object *copy = untag_ptr<Object>(target);
			WeakBlock *cwb = copy->weak_block;
			if (!cwb || get_ptr_tag(cwb) == Tag::OBJECT) { // has no wb yet
				cwb = new WeakBlock(cwb, COUNTER_STEP | OWNED);
				copy->weak_block = tag_ptr<WeakBlock>(cwb, Tag::WEAK_BLOCK);
			} else {
				cwb = untag_ptr<WeakBlock>(cwb);
			}
			dst = Object::retain(cwb);
			} break;
		}
	}
}

} // namespace ltm
