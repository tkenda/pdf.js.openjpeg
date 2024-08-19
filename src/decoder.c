/*
 * Copyright (c) 2024, Mozilla Foundation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY buffer OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

// clang-format off
#include "emscripten.h"
#include "openjpeg.h"
#include "color.h"
#include "convert.h"
// clang-format on

#define JP2_RFC3745_MAGIC_0 0x0C000000
#define JP2_RFC3745_MAGIC_1 0x2020506A
#define JP2_RFC3745_MAGIC_2 0x0A870A0D
#define JP2_MAGIC 0x0A870A0D
#define J2K_CODESTREAM_MAGIC 0x51FF4FFF

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

// 1 MiB pixels should be enough in most of the cases.
#define BUFFER_PIXELS_NUMBER 1048576

// #define PDFJS_DEBUG

extern void jsPrintWarning(const char *);
extern void setImageData(OPJ_UINT8 *, OPJ_SIZE_T, OPJ_SIZE_T);
extern void storeErrorMessage(const char *);
extern void copy_pixels_1(OPJ_INT32 *, OPJ_UINT32, OPJ_SIZE_T);
extern void copy_pixels_3(OPJ_INT32 *, OPJ_INT32 *, OPJ_INT32 *, OPJ_SIZE_T);
extern void copy_pixels_4(OPJ_INT32 *, OPJ_INT32 *, OPJ_INT32 *, OPJ_INT32 *,
                          OPJ_SIZE_T);

static void error_callback(const char *msg, void *client_data) {
  (void)client_data;
  storeErrorMessage(msg);
}
static void warning_callback(const char *msg, void *client_data) {
  (void)client_data;
  jsPrintWarning(msg);
}

static void quiet_callback(const char *msg, void *client_data) {
  (void)msg;
  (void)client_data;
}

int EMSCRIPTEN_KEEPALIVE jp2_decode(OPJ_UINT8 *data, OPJ_SIZE_T data_size,
                                    OPJ_UINT32 pdf_numcomps,
                                    OPJ_BOOL pdf_is_indexed_colormap,
                                    OPJ_BOOL pdf_smaks_in_data) {
  opj_dparameters_t parameters;
  opj_codec_t *l_codec = NULL;
  opj_image_t *image = NULL;
  opj_stream_t *l_stream = NULL;

  OPJ_UINT32 *int32data = (OPJ_UINT32 *)data;
  if (int32data[0] == JP2_MAGIC || (int32data[0] == JP2_RFC3745_MAGIC_0 &&
                                    int32data[1] == JP2_RFC3745_MAGIC_1 &&
                                    int32data[2] == JP2_RFC3745_MAGIC_2)) {
    l_codec = opj_create_decompress(OPJ_CODEC_JP2);
  } else if (int32data[0] == J2K_CODESTREAM_MAGIC) {
    l_codec = opj_create_decompress(OPJ_CODEC_J2K);
  } else {
    storeErrorMessage("Unknown format");
    return 1;
  }

  opj_set_info_handler(l_codec, quiet_callback, 00);
  opj_set_warning_handler(l_codec, warning_callback, 00);
  opj_set_error_handler(l_codec, error_callback, 00);

  opj_set_default_decoder_parameters(&parameters);
  if (pdf_is_indexed_colormap) {
    parameters.flags |= OPJ_DPARAMETERS_IGNORE_PCLR_CMAP_CDEF_FLAG;
  }

  // set stream
  opj_buffer_info_t buffer_info;
  buffer_info.buf = data;
  buffer_info.cur = data;
  buffer_info.len = data_size;
  l_stream = opj_stream_create_buffer_stream(&buffer_info, OPJ_TRUE);

  /* Setup the decoder decoding parameters using user parameters */
  if (unlikely(!opj_setup_decoder(l_codec, &parameters))) {
    storeErrorMessage("Failed to setup the decoder");
    opj_stream_destroy(l_stream);
    opj_destroy_codec(l_codec);
    return 1;
  }

  /* Read the main header of the codestream and if necessary the JP2 boxes*/
  if (unlikely(!opj_read_header(l_stream, l_codec, &image))) {
    storeErrorMessage("Failed to read the header");
    opj_stream_destroy(l_stream);
    opj_destroy_codec(l_codec);
    opj_image_destroy(image);
    return 1;
  }

  /* decode the image */
  if (unlikely(!opj_decode(l_codec, l_stream, image) ||
               !opj_end_decompress(l_codec, l_stream))) {
    storeErrorMessage("Failed to decode the image");
    opj_destroy_codec(l_codec);
    opj_stream_destroy(l_stream);
    opj_image_destroy(image);
    return 1;
  }

  opj_stream_destroy(l_stream);
  opj_destroy_codec(l_codec);

  if (image->icc_profile_buf) {
    // Avoid a memory leak (see opj_decompress.c).
    free(image->icc_profile_buf);
    image->icc_profile_buf = NULL;
    image->icc_profile_len = 0;
  }

  OPJ_UINT32 numcomps = image->numcomps;
  OPJ_SIZE_T nb_pixels = image->x1 * image->y1;

  /*
  printf("image X %d\n", image->x1);
  printf("image Y %d\n", image->y1);
  printf("image numcomps %d\n", image->numcomps);
  */

  switch (numcomps) {
    case 1:
      copy_pixels_1(image->comps[0].data, image->comps[0].prec, nb_pixels);
      break;
    case 3:
      copy_pixels_3(image->comps[0].data, image->comps[1].data,
                    image->comps[2].data, nb_pixels);
      break;
    case 4:
      copy_pixels_4(image->comps[0].data, image->comps[1].data,
                    image->comps[2].data, image->comps[3].data, nb_pixels);
      break;
  }

  opj_image_destroy(image);

  return 0;
}
