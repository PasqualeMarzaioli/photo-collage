/*
 * Single translation unit that compiles the stb single-header libraries.
 *
 * The stb headers ship both the declarations and the implementation in one
 * file; the implementation is emitted only where the matching IMPLEMENTATION
 * macro is defined. Keeping that here means every other source file can include
 * the headers for their declarations without producing duplicate symbols.
 *
 * Author: Pasquale Marzaioli
 */

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "stb_image.h"
#include "stb_image_write.h"
