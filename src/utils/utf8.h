#ifndef AK_UTF8_H__
#define AK_UTF8_H__

inline int get_utf8_no_surrogates(const char** ptr) {
	int r, n;
restart_and_reload:
	r = **ptr;
	(*ptr)++;
restart:
	n = 2;
	if ((r & 0x80) == 0)
		return r;
	if ((r & 0xe0) == 0xc0) r &= 0x1f;
	else if ((r & 0xf0) == 0xe0) n = 3, r &= 0xf;
	else if ((r & 0xf8) == 0xf0) n = 4, r &= 7;
	else
		goto restart_and_reload;
	while (--n) {
		int c = **ptr;
		(*ptr)++;
		if ((c & 0xc0) != 0x80) {
			r = c;
			goto restart;
		}
		r = r << 6 | (c & 0x3f);
	}
	return r;
}

inline int get_utf8(const char** ptr) {
	int r = get_utf8_no_surrogates(ptr);
	for (;;) {
		if (r < 0xD800 || r > 0xDFFF)
			return r;
		else if (r > 0xDBFF) // second part without first
			r = get_utf8_no_surrogates(ptr);
		else {
			int low_part = get_utf8_no_surrogates(ptr);
			if (low_part < 0xDC00 || low_part > 0xDFFF)
				r = low_part; // bad second part, restart
			else
				return ((r & 0x3ff) << 10 | (low_part & 0x3ff)) + 0x10000;
		}
	}
}

int put_utf8(int v, void* ctx, int(*put_fn)(void*, int)) {
	if (v <= 0x7f)
		return put_fn(ctx, v);
	else {
		int r;
		if (v <= 0x7ff)
			r = put_fn(ctx, v >> 6 | 0xc0);
		else {
			if (v <= 0xffff)
				r = put_fn(ctx, v >> (6 + 6) | 0xe0);
			else {
				if (v <= 0x10ffff)
					r = put_fn(ctx, v >> (6 + 6 + 6) | 0xf0);
				else
					return 0;
				if (r > 0)
					r = put_fn(ctx, ((v >> (6 + 6)) & 0x3f) | 0x80);
			}
			if (r > 0)
				r = put_fn(ctx, ((v >> 6) & 0x3f) | 0x80);
		}
		if (r > 0)
			r = put_fn(ctx, (v & 0x3f) | 0x80);
		return r;
	}
}

#endif // AK_UTF8_H__
