#include "video_source.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

int main() {
    const int width = 320;
    const int height = 240;

    printf("=== JPEG Encoder Verification Test ===\n");
    printf("Image: %dx%d (4:2:0)\n\n", width, height);

    VideoSource vs(width, height);

    printf("Generating frame via get_frame_jpeg()...\n");
    std::vector<uint8_t> jpeg = vs.get_frame_jpeg();
    printf("JPEG file size: %zu bytes\n", jpeg.size());

    if (jpeg.empty()) {
        printf("ERROR: JPEG is empty!\n");
        return 1;
    }

    bool has_soi = (jpeg.size() >= 2 &&
                    jpeg[0] == 0xFF && jpeg[1] == 0xD8);
    bool has_eoi = (jpeg.size() >= 2 &&
                    jpeg[jpeg.size() - 2] == 0xFF &&
                    jpeg[jpeg.size() - 1] == 0xD9);
    printf("SOI (FF D8): %s\n", has_soi ? "FOUND" : "MISSING");
    printf("EOI (FF D9): %s\n", has_eoi ? "FOUND" : "MISSING");

    bool has_dqt = false, has_sof = false, has_dht = false, has_sos = false;
    int dqt_count = 0, dht_count = 0;
    for (size_t i = 0; i + 1 < jpeg.size(); i++) {
        if (jpeg[i] == 0xFF) {
            uint8_t marker = jpeg[i + 1];
            if (marker == 0xDB) { has_dqt = true; dqt_count++; }
            if (marker == 0xC0) has_sof = true;
            if (marker == 0xC4) { has_dht = true; dht_count++; }
            if (marker == 0xDA) has_sos = true;
        }
    }
    printf("DQT (FF DB): %s (count=%d)\n", has_dqt ? "FOUND" : "MISSING", dqt_count);
    printf("SOF0 (FF C0): %s\n", has_sof ? "FOUND" : "MISSING");
    printf("DHT (FF C4): %s (count=%d)\n", has_dht ? "FOUND" : "MISSING", dht_count);
    printf("SOS (FF DA): %s\n", has_sos ? "FOUND" : "MISSING");

    printf("\nFirst 64 bytes (hex):\n");
    for (size_t i = 0; i < jpeg.size() && i < 64; i++) {
        printf("%02X ", jpeg[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");

    const char* filename = "test_output.jpg";
    FILE* f = fopen(filename, "wb");
    if (!f) {
        printf("ERROR: Cannot open %s for writing\n", filename);
        return 1;
    }
    fwrite(jpeg.data(), 1, jpeg.size(), f);
    fclose(f);
    printf("Saved to: %s\n", filename);

    printf("\n=== Verifying with ffmpeg ===\n");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ffmpeg -v error -i %s -f null - 2>&1", filename);
    int ret = system(cmd);
    if (ret == 0) {
        printf("ffmpeg verification: PASSED (JPEG is valid!)\n");
    } else {
        printf("ffmpeg verification: FAILED (JPEG may be invalid)\n");
        printf("Trying ffplay...\n");
        snprintf(cmd, sizeof(cmd), "ffplay -v error -frames 1 %s 2>&1", filename);
        ret = system(cmd);
        if (ret == 0) {
            printf("ffplay verification: PASSED\n");
        } else {
            printf("ffplay verification: FAILED\n");
        }
    }

    if (has_soi && has_eoi && has_dqt && has_sof && has_dht && has_sos && dqt_count == 2 && dht_count == 4) {
        printf("\nJPEG STRUCTURE: VALID\n");
    } else {
        printf("\nJPEG STRUCTURE: INCOMPLETE or INCORRECT\n");
        if (dqt_count != 2) printf("  Expected 2 DQT segments, got %d\n", dqt_count);
        if (dht_count != 4) printf("  Expected 4 DHT segments, got %d\n", dht_count);
    }

    printf("\n=== Testing 3 consecutive frames ===\n");
    for (int i = 0; i < 3; i++) {
        std::vector<uint8_t> frame = vs.get_frame_jpeg();
        printf("Frame %d: %zu bytes, SOI=%d, EOI=%d\n",
               i, frame.size(),
               frame[0] == 0xFF && frame[1] == 0xD8,
               frame[frame.size()-2] == 0xFF && frame[frame.size()-1] == 0xD9);
    }

    return 0;
}