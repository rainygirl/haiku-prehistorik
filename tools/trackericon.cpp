// Prints and dumps the icon Tracker would draw for a path, so icon problems
// can be checked without looking at the screen.
// Usage: trackericon <path> [out.ppm]
#include <Bitmap.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Application.h>
#include <Entry.h>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s <path> [out.ppm]\n", argv[0]); return 1; }
	BApplication app("application/x-vnd.rainygirl-trackericon");
	BNode node(argv[1]);
	if (node.InitCheck() != B_OK) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
	BNodeInfo info(&node);
	char type[B_MIME_TYPE_LENGTH] = "";
	info.GetType(type);
	printf("path: %s\ntype: %s\n", argv[1], type[0] ? type : "(none)");
	BBitmap bitmap(BRect(0, 0, 31, 31), B_RGBA32);
	status_t s = info.GetTrackerIcon(&bitmap, B_LARGE_ICON);
	printf("GetTrackerIcon: %s\n", strerror(s));
	if (s != B_OK) return 1;
	const uint8_t* bits = static_cast<const uint8_t*>(bitmap.Bits());
	int stride = int(bitmap.BytesPerRow());
	// A quick fingerprint so two icons can be compared without looking.
	uint32_t sum = 0, opaque = 0;
	for (int y = 0; y < 32; ++y)
		for (int x = 0; x < 32; ++x) {
			const uint8_t* p = bits + y * stride + x * 4;
			sum = sum * 31 + p[0] + p[1] * 3 + p[2] * 7 + p[3] * 11;
			if (p[3] > 128) ++opaque;
		}
	printf("fingerprint: %08x  opaque pixels: %u\n", sum, opaque);
	if (argc > 2) {
		FILE* f = fopen(argv[2], "wb");
		if (f) {
			fprintf(f, "P6\n32 32\n255\n");
			for (int y = 0; y < 32; ++y)
				for (int x = 0; x < 32; ++x) {
					const uint8_t* p = bits + y * stride + x * 4;
					// composite over white so the shape is visible
					uint8_t a = p[3];
					uint8_t rgb[3] = { uint8_t((p[2] * a + 255 * (255 - a)) / 255),
					                   uint8_t((p[1] * a + 255 * (255 - a)) / 255),
					                   uint8_t((p[0] * a + 255 * (255 - a)) / 255) };
					fwrite(rgb, 1, 3, f);
				}
			fclose(f);
			printf("wrote %s\n", argv[2]);
		}
	}
	return 0;
}
