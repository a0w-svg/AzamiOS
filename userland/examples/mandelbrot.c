#include <stdio.h>

int main(void) {
    const int width = 60;
    const int height = 24;
    const int max_iter = 64;
    const char charset[] = " .:-=+*#%@";
    const int charset_len = sizeof(charset) - 2;

    printf("\n--- ASCII Mandelbrot Fractal (AzamiOS GCC) ---\n\n");

    for (int y = 0; y < height; y++) {
        double cy = (y - height / 2.0) * 2.0 / height;
        for (int x = 0; x < width; x++) {
            double cx = (x - width / 1.5) * 2.5 / width;
            double zx = 0.0, zy = 0.0;
            int iter = 0;

            while (zx * zx + zy * zy < 4.0 && iter < max_iter) {
                double tmp = zx * zx - zy * zy + cx;
                zy = 2.0 * zx * zy + cy;
                zx = tmp;
                iter++;
            }

            if (iter >= max_iter) {
                putchar('#');
            } else {
                int char_idx = (iter * charset_len) / max_iter;
                putchar(charset[char_idx]);
            }
        }
        putchar('\n');
    }

    printf("\nMandelbrot render complete.\n");
    return 0;
}
