
#include <string.h>
#include <cmath>
#include <vector>
#include "resize.h"

namespace picha {

	using std::vector;

	typedef vector<float> PixelContribs;

	struct ContribRange {
		int left, right, weights;
	};

	typedef vector<ContribRange> RangeVector;

	template <typename Filter> void makeContribs(RangeVector & ranges, const Filter & filter,
		float scale, PixelContribs& storage, int size) {

		float fscale(std::max(std::max(scale, 1.0f), 1.0f / filter.support()));
		float fsupport(filter.support() * fscale);
		float iscale(1.0f / fscale);

		float center = float(0.5) * scale;
		for (RangeVector::iterator i = ranges.begin(); i != ranges.end(); ++i, center += scale) {
			float totalweight(0);
			int left = int(std::max(float(0), std::ceil(center - fsupport)));
			int right = int(std::min(float(size - 1), std::floor(center + fsupport)));
			while (left < right && filter((center - left) * iscale) == 0)
				left += 1;
			while (right > left && filter((center - right) * iscale) == 0)
				right -= 1;
			i->left = left;
			i->right = right;
			i->weights = storage.size();
			for (int j = left; j <= right; ++j) {
				float o = center - float(j);
				float w = filter(o * iscale);
				storage.push_back(w);
				totalweight += w;
			}

			assert(totalweight > 0);
			float normalize = float(1) / totalweight;
			for (int j = i->weights; j != int(storage.size()); ++j)
				storage[j] *= normalize;
		}
	}

	struct FloatBuffer {
		FloatBuffer(int w, int h, int d) {
			width = w;
			height = h;
			stride = w * d;
			data.resize(stride * height);
		}

		float *row(int y) { return &data[y * stride]; }

		int width, height, stride;
		vector<float> data;
	};

	inline PixelType clampPixel(float v) {
		return v < 0 ? 0 : v > 255 ? 255 : PixelType(v);
	}

	template <PixelMode Pixel, typename Filter>
	void resizeImagePixel(NativeImage & src, NativeImage & dst, const Filter & filter) {
		assert(src.pixel == Pixel);
		assert(dst.pixel == Pixel);

		// Scale and support values.
		float xscale = src.width / float(dst.width);
		float yscale = src.height / float(dst.height);
		float xfscale = std::max(std::max(xscale, 1.0f), 1.0f / filter.support());
		float yfscale = std::max(std::max(yscale, 1.0f), 1.0f / filter.support());
		float xfsupport = filter.support() * xfscale;
		float yfsupport = filter.support() * yfscale;
		int maxxcontrib = int(std::ceil(2 * xfsupport));
		int maxycontrib = int(std::ceil(2 * yfsupport));

		// A temporary image to hold resized rows as we move through the image.
		const int pixelWidth = PixelWidth<Pixel>::value;
		FloatBuffer tmp(dst.width, maxycontrib, pixelWidth);

		// Buffer to hold pre-calculated weights
		PixelContribs contribs;
		contribs.reserve(maxxcontrib * dst.width + maxycontrib * dst.height);

		// Pre-computed source contributions for the rows.
		RangeVector rowcontribs;
		rowcontribs.resize(dst.width);
		makeContribs(rowcontribs, filter, xscale, contribs, src.width);

		// Pre-computed source contributions for the columns.
		RangeVector columncontribs;
		columncontribs.resize(dst.height);
		makeContribs(columncontribs, filter, yscale, contribs, src.height);

		float centery = float(0.5) * yscale;
		int srcrow(int(std::max(float(0), std::ceil(centery - yfsupport))));
		for (int y = 0; y < dst.height; ++y, centery += yscale) {

			// Resize any source rows needed for this row of the destination.
			int needrow(std::min(src.height - 1, int(centery + yfsupport)));
			while (srcrow <= needrow) {
				PixelType * srcdata = src.row(srcrow);
				float * tmprow = tmp.row(srcrow % maxycontrib);
				memset(tmprow, 0, sizeof(float) * pixelWidth * tmp.width);
				for (int x = 0; x < tmp.width; ++x) {
					for (int c = rowcontribs[x].left, w = rowcontribs[x].weights; c <= rowcontribs[x].right; ++c, ++w) {
						PixelType * srcpixel = &srcdata[c * pixelWidth];
						for (int p = 0; p < pixelWidth; ++p)
							tmprow[p] += contribs[w] * srcpixel[p];
					}
					tmprow += pixelWidth;
				}
				++srcrow;
			}

			// Resize this row of the destination using the resized temporary rows.
			PixelType * dstrow = dst.row(y);
			for (int x = 0; x < dst.width; ++x) {
				float pixel[pixelWidth] = {};
				for (int c = columncontribs[y].left, w = columncontribs[y].weights; c <= columncontribs[y].right; ++c, ++w) {
					float * srcpixel = tmp.row(c % maxycontrib) + x * pixelWidth;
					for (int p = 0; p < pixelWidth; ++p)
						pixel[p] += contribs[w] * srcpixel[p];
				}
				for (int p = 0; p < pixelWidth; ++p)
					dstrow[p] = pixel[p] < 0.0f ? PixelType(0) :
								pixel[p] > 255.0f ? PixelType(255) :
								PixelType(pixel[p] + 0.5f);
				dstrow += pixelWidth;
			}
		}
	}

	template <typename Filter> void resizeImage(NativeImage& src, NativeImage& dst, const Filter& filter) {
		assert(src.pixel == dst.pixel);
		switch (src.pixel) {
			case RGBA_PIXEL : resizeImagePixel<RGBA_PIXEL>(src, dst, filter); break;
			case RGB_PIXEL : resizeImagePixel<RGB_PIXEL>(src, dst, filter); break;
			case GREY_PIXEL : resizeImagePixel<GREY_PIXEL>(src, dst, filter); break;
			case GREYA_PIXEL : resizeImagePixel<GREYA_PIXEL>(src, dst, filter); break;
			default : assert(false);
		}
	}

	enum ResizeFilterTag {
		CubicFilterTag,
		LanczosFilterTag,
		CatmulRomFilterTag,
		MitchelFilterTag,
		BoxFilterTag,
		TriangleFilterTag,

		InvalidFilterTag,
	};

	static Persistent<String>* const resizeFilterSymbols[] = {
		&cubic_symbol, &lanczos_symbol, &catmulrom_symbol, &mitchel_symbol, &box_symbol, &triangle_symbol
	};

	ResizeFilterTag symbolToResizeFilter(Handle<Value> s) {
		for (int i = 0; i < InvalidFilterTag; ++i)
			if (s->StrictEquals(NanNew(*resizeFilterSymbols[i])))
				return static_cast<ResizeFilterTag>(i);
		return InvalidFilterTag;
	}

	struct ResizeOptions {
		ResizeOptions() : filter(CubicFilterTag), width(0.70f) {}
		ResizeFilterTag filter;
		float width;
	};

	bool getResizeOptions(ResizeOptions& s, Handle<Object> opts) {
		Local<Value> v = opts->Get(NanNew(filter_symbol));
		if (!v->IsUndefined()) {
			s.width = 1.0f;
			s.filter = symbolToResizeFilter(opts->Get(NanNew(filter_symbol)));
			if (s.filter == InvalidFilterTag) {
				NanThrowError("invalid filter mode");
				return false;
			}
		}
		v = opts->Get(NanNew(filterScale_symbol));
		if (!v->IsUndefined()) {
			s.width = v->NumberValue();
			if (s.width != s.width || s.width <= 0) {
				NanThrowError("invalid filter width");
				return false;
			}
		}
		return true;
	}

	struct TriangleFilter {
		float support() const { return 1.0f; }
		float operator () (float o) const { return 1.0f - std::abs(o); }
	};

	struct BoxFilter {
		float support() const { return 0.5f; }
		float operator () (float o) const { return 1.0f; }
	};

	template <typename Base> struct MitchelFamilyFilter {
		float support() const { return 2.0f; }

		float operator () (float o) const {
			const float B = Base::B();
			const float C = Base::C();
			float x = std::abs(o);
			if (x < 1) {
				const float A_3 = (12 - 9 * B - 6 * C) / 6;
				const float A_2 = (-18 + 12 * B + 6 * C) / 6;
				const float A_0 = (6 - 2 * B) / 6;
				return A_0 + (x * x * (A_2 + x * A_3));
			}
			else {
				const float B_3 = (-B - 6 * C) / 6;
				const float B_2 = (6 * B + 30 * C) / 6;
				const float B_1 = (-12 * B - 48 * C) / 6;
				const float B_0 = (8 * B + 24 * C) / 6;
				return B_0 + (x * (B_1 + x * (B_2 + x * B_3)));
			}
		}
	};

	struct CatmulRomParams {
		static float B() { return 0.0f; }
		static float C() { return 0.5f; }
	};

	struct MitchelParams {
		static float B() { return 0.333f; }
		static float C() { return 0.333f; }
	};

	typedef MitchelFamilyFilter<CatmulRomParams> CatmulRomFilter;
	typedef MitchelFamilyFilter<MitchelParams> MitchelFilter;

	template <unsigned A> struct LanczosFamilyFilter {
		float support() const { return float(A); }

		float operator () (float o) const {
			float x = o * float(M_PI), x2 = x * x;
			return x2 == 0 ? 1.0f : A * std::sin(x) * std::sin(x / A) / x2;
		}
	};

	typedef LanczosFamilyFilter<2> LanczosFilter;

	struct CubicFilter {
		float support() const { return 2.0f; }
		float operator () (float o) const { o = std::abs(o); return 1.0f - o * o * (0.75f - 0.25f * o); }
	};

	template <typename F> struct ScaledFilter {
		F filter;
		float scale;
		ScaledFilter(float s) : scale(s) {}
		float support() const { return scale * filter.support(); }
		float operator () (float f) const { return filter(f / scale) / scale; }
	};

	void resizeImage(const ResizeOptions& opts, NativeImage& src, NativeImage& dst) {
		switch (opts.filter) {
			case CubicFilterTag : resizeImage(src, dst, ScaledFilter<CubicFilter>(opts.width)); break;
			case LanczosFilterTag : resizeImage(src, dst, ScaledFilter<LanczosFilter>(opts.width)); break;
			case CatmulRomFilterTag : resizeImage(src, dst, ScaledFilter<CatmulRomFilter>(opts.width)); break;
			case MitchelFilterTag : resizeImage(src, dst, ScaledFilter<MitchelFilter>(opts.width)); break;
			case BoxFilterTag : resizeImage(src, dst, ScaledFilter<BoxFilter>(opts.width)); break;
			case TriangleFilterTag : resizeImage(src, dst, ScaledFilter<TriangleFilter>(opts.width)); break;
			default: assert(false);
		}
	}

	struct ResizeContext {
		Persistent<Value> srcbuffer;
		Persistent<Object> dstimage;
		Persistent<Function> cb;
		ResizeOptions opts;
		NativeImage src;
		NativeImage dst;
	};

	void UV_resize(uv_work_t* work_req) {
		ResizeContext *ctx = reinterpret_cast<ResizeContext*>(work_req->data);
		resizeImage(ctx->opts, ctx->src, ctx->dst);
	}

	void V8_resize(uv_work_t* work_req, int) {
		NanScope();

		ResizeContext *ctx = reinterpret_cast<ResizeContext*>(work_req->data);

		Local<Value> dst = NanNew(ctx->dstimage);
		Local<Function> cb = NanNew<Function>(ctx->cb);
		NanDisposePersistent(ctx->srcbuffer);
		NanDisposePersistent(ctx->dstimage);
		NanDisposePersistent(ctx->cb);
		delete work_req;
		delete ctx;

		TryCatch try_catch;

		Handle<Value> argv[2] = { NanUndefined(), dst };
		NanMakeCallback(NanGetCurrentContext()->Global(), cb, 2, argv);

		if (try_catch.HasCaught())
			FatalException(try_catch);

		return;
	}

	NAN_METHOD(resize) {
		NanScope();

		if (args.Length() != 3 || !args[0]->IsObject() || !args[1]->IsObject() || !args[2]->IsFunction()) {
			NanThrowError("expected: resize(image, opts, cb)");
			NanReturnUndefined();
		}
		Local<Object> img = args[0]->ToObject();
		Local<Object> opts = args[1]->ToObject();
		Local<Function> cb = Local<Function>::Cast(args[2]);

		NativeImage src;
		src = jsImageToNativeImage(img);
		if (!src.data) {
			NanThrowError("invalid image");
			NanReturnUndefined();
		}

		int width = opts->Get(NanNew(width_symbol))->Uint32Value();
		int height = opts->Get(NanNew(height_symbol))->Uint32Value();
		if (width <= 0 || height <= 0) {
			NanThrowError("invalid dimensions");
			NanReturnUndefined();
		}

		ResizeOptions rsopts;
		if (!getResizeOptions(rsopts, opts)) {
			NanReturnUndefined();
		}

		ResizeContext * ctx = new ResizeContext;
		Local<Object> jsdst = newJsImage(width, height, src.pixel);
		NanAssignPersistent(ctx->srcbuffer, img->Get(NanNew(data_symbol)));
		NanAssignPersistent(ctx->dstimage, jsdst);
		NanAssignPersistent(ctx->cb, cb);
		ctx->dst = jsImageToNativeImage(jsdst);
		ctx->opts = rsopts;
		ctx->src = src;

		uv_work_t* work_req = new uv_work_t();
		work_req->data = ctx;
		uv_queue_work(uv_default_loop(), work_req, UV_resize, V8_resize);

		NanReturnUndefined();
	}

	NAN_METHOD(resizeSync) {
		NanScope();

		if (args.Length() != 2 || !args[0]->IsObject() || !args[1]->IsObject()) {
			NanThrowError("expected: resizeSync(image, opts)");
			NanReturnUndefined();
		}
		Local<Object> img = args[0]->ToObject();
		Local<Object> opts = args[1]->ToObject();

		NativeImage src;
		src = jsImageToNativeImage(img);
		if (!src.data) {
			NanThrowError("invalid image");
			NanReturnUndefined();
		}

		int width = opts->Get(NanNew(width_symbol))->Uint32Value();
		int height = opts->Get(NanNew(height_symbol))->Uint32Value();
		if (width <= 0 || height <= 0) {
			NanThrowError("invalid dimensions");
			NanReturnUndefined();
		}

		ResizeOptions rsopts;
		if (!getResizeOptions(rsopts, opts)) {
			NanReturnUndefined();
		}

		Local<Object> jsdst = newJsImage(width, height, src.pixel);
		NativeImage dst = jsImageToNativeImage(jsdst);

		resizeImage(rsopts, src, dst);

		NanReturnValue(jsdst);
	}

}
