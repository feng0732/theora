/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggTheora SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE Theora SOURCE CODE IS COPYRIGHT (C) 2002-2009                *
 * by the Xiph.Org Foundation https://www.xiph.org/                 *
 *                                                                  *
 ********************************************************************

  function: routines for validating encoder granulepos generation

 ********************************************************************/

#include <stdlib.h>
#include <string.h>
#include <theora/theoraenc.h>
#include <theora/theoradec.h>
#include <math.h>

#include "tests.h"

static int ilog(unsigned int v){
  int ret=0;
  while(v){
    ret++;
    v>>=1;
  }
  return(ret);
}

static void
granulepos_fill_info (th_info *ti, int frequency)
{
  th_info_init (ti);

  ti->frame_width = 32;
  ti->frame_height = 32;
  ti->pic_width = ti->frame_width;
  ti->pic_height = ti->frame_height;
  ti->pic_x = 0;
  ti->pic_y = 0;
  ti->fps_numerator = 16;
  ti->fps_denominator = 1;
  ti->aspect_numerator = 1;
  ti->aspect_denominator = 1;
  ti->colorspace = TH_CS_UNSPECIFIED;
  ti->pixel_fmt = TH_PF_420;
  ti->quality = 16;
  ti->keyframe_granule_shift=ilog(frequency);
}

/*Feed the three header packets emitted by the encoder to a fresh decoder.
  The returned decoder is ready to accept data packets.*/
static th_dec_ctx *
granulepos_open_decoder (th_enc_ctx *te, th_info *dti, th_comment *dtc)
{
  th_comment enc_tc;
  th_setup_info *setup;
  ogg_packet op;
  th_dec_ctx *td;
  int result;

  setup = NULL;
  th_comment_init (&enc_tc);
  th_comment_init (dtc);
  th_info_init (dti);
  while (th_encode_flushheader (te, &enc_tc, &op) > 0) {
    result = th_decode_headerin (dti, dtc, &setup, &op);
    if (result < 0) {
      printf ("th_decode_headerin() returned %d\n", result);
      FAIL ("failed to parse an encoder header packet");
    }
  }
  th_comment_clear (&enc_tc);
  td = th_decode_alloc (dti, setup);
  if (td == NULL)
    FAIL ("th_decode_alloc() returned a null pointer");
  th_setup_free (setup);
  return td;
}

static int
granulepos_test_encode (int frequency)
{
  th_info ti;
  th_enc_ctx *te;
  int result;
  int frame, tframe, keyframe, keydist;
  int shift;
  double rate, ttime;
  th_ycbcr_buffer yuv;
  unsigned char *framedata;
  ogg_packet op;
  long long int last_granule = -1;

  granulepos_fill_info (&ti, frequency);

/*  INFO ("+ Allocating encoder context"); */
  te = th_encode_alloc(&ti);
  if (te == NULL) {
    INFO ("+ Clearing th_info");
    th_info_clear(&ti);
    FAIL ("negative return code initializing encoder");
  }

/*  INFO ("+ Setting up dummy 4:2:0 frame data"); */
  framedata = calloc(ti.frame_height, ti.frame_width);
  yuv[0].width = ti.frame_width;
  yuv[0].height = ti.frame_height;
  yuv[0].stride = ti.frame_width;
  yuv[0].data = framedata;
  yuv[1].width = ti.frame_width / 2;
  yuv[1].height = ti.frame_width / 2;
  yuv[1].stride = ti.frame_width;
  yuv[1].data = framedata;
  yuv[2].width = ti.frame_width / 2;
  yuv[2].height = ti.frame_width / 2;
  yuv[2].stride = ti.frame_width;
  yuv[2].data = framedata;

  INFO ("+ Checking granulepos generation");
  shift = ti.keyframe_granule_shift;
  rate = (double)ti.fps_denominator/ti.fps_numerator;
  for (frame = 0; frame < frequency * 2 + 1; frame++) {
    result = th_encode_ycbcr_in (te, yuv);
    if (result < 0) {
      printf("th_encode_ycbcr_in() returned %d\n", result);
      FAIL ("negative error code submitting frame for compression");
    }
    result = th_encode_packetout (te, frame >= frequency * 2, &op);
    if (result <= 0) {
      printf("th_encode_packetout() returned %d\n", result);
      FAIL("failed to retrieve compressed frame");
    }
    if ((long long int)op.granulepos < last_granule)
      FAIL ("encoder returned a decreasing granulepos value");
    last_granule = op.granulepos;
    keyframe = op.granulepos >> shift;
    keydist = op.granulepos - (keyframe << shift);
    tframe = th_granule_frame (te, op.granulepos);
    ttime = th_granule_time(te, op.granulepos);
#if DEBUG
    printf("++ frame %d granulepos %lld %d:%d %d %.3lfs\n",
	frame, (long long int)op.granulepos, keyframe, keydist,
	tframe, th_granule_time (te, op.granulepos));
#endif
    /*The first packet is always a keyframe: the keyframe base holds the
      frame count (1) and the intra-frame offset is zero.*/
    if (frame == 0) {
      if (!th_packet_iskeyframe (&op))
        FAIL ("first data packet was not marked as a keyframe");
      if (keyframe != 1 || keydist != 0)
        FAIL ("first frame granulepos does not encode a 1:0 keyframe base");
      if ((long long int)op.granulepos != 1LL << shift)
        FAIL ("first frame granulepos does not equal the shifted frame count");
    }
    if (th_packet_iskeyframe (&op)) {
      /*A keyframe packet carries no intra-frame offset.*/
      if (keydist != 0)
        FAIL ("keyframe granulepos has a non-zero intra-frame offset");
      if (keyframe != frame + 1)
        FAIL ("keyframe base does not hold the frame count");
    }
    else {
      /*For an inter frame the high part is the last keyframe's frame count,
        not the global frame number: using it alone would address an earlier
        frame.*/
      if (keydist <= 0)
        FAIL ("inter frame granulepos has no intra-frame offset");
      if (keyframe >= frame + 1)
        FAIL ("keyframe base misread as the global frame number");
      if (th_granule_frame (te, (ogg_int64_t)keyframe << shift) !=
       keyframe - 1)
        FAIL ("keyframe base alone does not resolve to the keyframe index");
      if (th_granule_frame (te, (ogg_int64_t)keyframe << shift) == frame)
        FAIL ("keyframe base must not be used as the global frame number");
    }
    /* granulepos stores the frame count */
    if ((keyframe + keydist) != frame + 1)
      FAIL ("encoder granulepos does not map to the correct frame number");
    /* th_granule_frame() returns the frame index */
    if (tframe != frame)
      FAIL ("th_granule_frame() returned incorrect results");
    /* packetno shares the same display ordering as the decoded frame index:
      the three header packets occupy packet numbers 0, 1 and 2.*/
    if (op.packetno != tframe + 3)
      FAIL ("encoder packetno does not track the decoded frame number");
    /* th_granule_time() returns the end time */
    if (fabs(rate*(frame+1) - ttime) > 1.0e-6)
      FAIL ("th_granule_time() returned incorrect results");
  }

  /* clean up */
/*  INFO ("+ Freeing dummy frame data"); */
  free(framedata);

/*  INFO ("+ Clearing th_info struct"); */
  th_info_clear(&ti);

/*  INFO ("+ Freeing encoder context"); */
  th_encode_free(te);

  return 0;
}

/*Validate streams containing duplicate frames: TH_ENCCTL_SET_DUP_COUNT makes
  the encoder emit extra zero-byte/duplicate packets whose granule positions
  must keep advancing through the display order, and a decoder must recover the
  same positions and frame numbers.
  The encoder only holds one batch of duplicates at a time, so its packets must
  be pumped out after each submitted frame.*/
static int
granulepos_test_duplicate_frames (void)
{
  static const int dup_counts[4] = { 2, 1, 0, 0 };
  static const int expect_dup[7] = { 0, 1, 1, 0, 1, 0, 0 };
  th_info ti;
  th_info dti;
  th_comment dtc;
  th_enc_ctx *te;
  th_dec_ctx *td;
  int result;
  int coded;
  int shift;
  int display;
  int rate;
  th_ycbcr_buffer yuv;
  unsigned char *framedata;
  ogg_packet op;
  ogg_int64_t dgp;
  long long int last_granule = -1;

  /*A large forced keyframe gap keeps every coded frame here inter except the
    first, and SET_DUP_COUNT requires a count strictly below it.*/
  granulepos_fill_info (&ti, 8);
  te = th_encode_alloc (&ti);
  if (te == NULL) {
    th_info_clear (&ti);
    FAIL ("negative return code initializing encoder");
  }
  td = granulepos_open_decoder (te, &dti, &dtc);

  framedata = calloc (ti.frame_height, ti.frame_width + 64);
  yuv[0].width = ti.frame_width;
  yuv[0].height = ti.frame_height;
  yuv[0].stride = ti.frame_width;
  yuv[0].data = framedata;
  yuv[1].width = ti.frame_width / 2;
  yuv[1].height = ti.frame_width / 2;
  yuv[1].stride = ti.frame_width;
  yuv[1].data = framedata + ti.frame_width * ti.frame_height;
  yuv[2].width = ti.frame_width / 2;
  yuv[2].height = ti.frame_width / 2;
  yuv[2].stride = ti.frame_width;
  yuv[2].data = framedata + ti.frame_width * ti.frame_height;
  shift = ti.keyframe_granule_shift;
  rate = ti.fps_numerator;

  /*Submit four coded frames, two of which carry duplicate frames, pumping the
    one coded packet plus its queued duplicate packets after each submission so
    the next submission cannot discard them.
    Give every coded frame distinct content so a real inter packet always
    contains coded blocks, while the scheduled duplicate frames are emitted as
    zero-byte packets.*/
  display = 0;
  for (coded = 0; coded < 4; coded++) {
    int out;
    memset (framedata, (coded + 1) * 17,
     (size_t)ti.frame_width * ti.frame_height);
    result = th_encode_ctl (te, TH_ENCCTL_SET_DUP_COUNT,
     (void *)&dup_counts[coded], sizeof (dup_counts[coded]));
    if (result < 0) {
      printf ("th_encode_ctl(TH_ENCCTL_SET_DUP_COUNT) returned %d\n", result);
      FAIL ("failed to queue duplicate frames");
    }
    result = th_encode_ycbcr_in (te, yuv);
    if (result < 0) {
      printf ("th_encode_ycbcr_in() returned %d\n", result);
      FAIL ("negative error code submitting frame for compression");
    }
    /*Each submission yields exactly its coded packet followed by its
      duplicate frames.*/
    for (out = 0; out < 1 + dup_counts[coded]; out++) {
      int keyframe;
      int keydist;
      int tframe;
      result = th_encode_packetout (te,
       coded == 3 && out == dup_counts[coded], &op);
      if (result <= 0) {
        printf ("th_encode_packetout() returned %d\n", result);
        FAIL ("failed to retrieve a coded or duplicate frame");
      }
      if ((long long int)op.granulepos <= last_granule)
        FAIL ("duplicate frame granulepos did not advance");
      last_granule = op.granulepos;
      keyframe = op.granulepos >> shift;
      keydist = op.granulepos - (keyframe << shift);
      tframe = th_granule_frame (te, op.granulepos);
      /*Every emitted packet, duplicates included, maps to the next display
        frame in order.*/
      if (keyframe + keydist != display + 1)
        FAIL ("duplicate frame granulepos does not map to display order");
      if (tframe != display)
        FAIL ("th_granule_frame() skipped a duplicate frame");
      if (op.packetno != tframe + 3)
        FAIL ("duplicate frame packetno does not track the frame number");
      if (fabs ((double)(display + 1) / rate -
       th_granule_time (te, op.granulepos)) > 1.0e-6)
        FAIL ("th_granule_time() returned incorrect results for a dup");
      /*Zero-byte packets are the explicit duplicate frame representation.*/
      if ((op.bytes == 0) != expect_dup[display])
        FAIL ("duplicate frame packet size does not match its display slot");

      /*The decoder must recover the identical granule position and frame
        number from the packet, and report duplicate frames as such.*/
      result = th_decode_packetin (td, &op, &dgp);
      if (expect_dup[display]) {
        if (result != TH_DUPFRAME)
          FAIL ("decoder did not flag a duplicate frame with TH_DUPFRAME");
      }
      else if (result < 0) {
        printf ("th_decode_packetin() returned %d\n", result);
        FAIL ("failed to decode a coded frame");
      }
      if (dgp != op.granulepos)
        FAIL ("decoder granulepos disagrees with the encoder position");
      if (th_granule_frame (td, dgp) != display)
        FAIL ("decoder frame number disagrees with the encoder order");
      if (fabs ((double)(display + 1) / rate -
       th_granule_time (td, dgp)) > 1.0e-6)
        FAIL ("decoder granule time disagrees with the encoder order");
      display++;
    }
  }
  if (display != 7)
    FAIL ("encoder did not emit every coded and duplicate frame");

  free (framedata);
  th_decode_free (td);
  th_info_clear (&dti);
  th_comment_clear (&dtc);
  th_encode_free (te);
  th_info_clear (&ti);
  return 0;
}

/*Unknown granule positions (negative values, the Ogg convention for "no
  position known yet") must keep an explicit return semantics instead of being
  decoded as frame data.*/
static int
granulepos_test_unknown (void)
{
  th_info ti;
  th_info dti;
  th_comment dtc;
  th_enc_ctx *te;
  th_dec_ctx *td;
  ogg_int64_t unknown;
  th_ycbcr_buffer yuv;
  unsigned char *framedata;

  granulepos_fill_info (&ti, 4);
  te = th_encode_alloc (&ti);
  if (te == NULL) {
    th_info_clear (&ti);
    FAIL ("negative return code initializing encoder");
  }
  td = granulepos_open_decoder (te, &dti, &dtc);
  framedata = calloc (ti.frame_height, ti.frame_width);
  yuv[0].width = ti.frame_width;
  yuv[0].height = ti.frame_height;
  yuv[0].stride = ti.frame_width;
  yuv[0].data = framedata;
  yuv[1].width = ti.frame_width / 2;
  yuv[1].height = ti.frame_width / 2;
  yuv[1].stride = ti.frame_width;
  yuv[1].data = framedata;
  yuv[2].width = ti.frame_width / 2;
  yuv[2].height = ti.frame_width / 2;
  yuv[2].stride = ti.frame_width;
  yuv[2].data = framedata;

  unknown = -1;
  /*Both the encoder and the decoder reject the unknown value up front.*/
  if (th_granule_frame (te, unknown) != -1)
    FAIL ("th_granule_frame() accepted an unknown encoder granulepos");
  if (th_granule_time (te, unknown) != -1.0)
    FAIL ("th_granule_time() accepted an unknown encoder granulepos");
  if (th_granule_frame (td, unknown) != -1)
    FAIL ("th_granule_frame() accepted an unknown decoder granulepos");
  if (th_granule_time (td, unknown) != -1.0)
    FAIL ("th_granule_time() accepted an unknown decoder granulepos");
  /*Seeking to an unknown position is itself invalid.*/
  if (th_decode_ctl (td, TH_DECCTL_SET_GRANPOS, &unknown,
   sizeof (unknown)) != TH_EINVAL)
    FAIL ("TH_DECCTL_SET_GRANPOS accepted a negative granulepos");

  /*A real, non-negative granule position still resolves normally, proving the
    -1 return above is specific to the unknown value.*/
  if (th_encode_ycbcr_in (te, yuv) < 0)
    FAIL ("negative error code submitting frame for compression");
  {
    ogg_packet op;
    if (th_encode_packetout (te, 1, &op) <= 0)
      FAIL ("failed to retrieve compressed frame");
    if (op.granulepos < 0)
      FAIL ("encoder emitted a packet with an unknown granulepos");
    if (th_granule_frame (te, op.granulepos) != 0)
      FAIL ("known granulepos no longer resolves to the first frame");
    if (fabs (th_granule_time (te, op.granulepos) -
     (double)ti.fps_denominator / ti.fps_numerator) > 1.0e-6)
      FAIL ("known granulepos no longer resolves to the first frame time");
  }

  free (framedata);
  th_decode_free (td);
  th_info_clear (&dti);
  th_comment_clear (&dtc);
  th_encode_free (te);
  th_info_clear (&ti);
  return 0;
}

int main(int argc, char *argv[])
{

  granulepos_test_encode (1);
  granulepos_test_encode (2);
  granulepos_test_encode (3);
  granulepos_test_encode (4);
  granulepos_test_encode (8);
  granulepos_test_encode (64);

  granulepos_test_duplicate_frames ();
  granulepos_test_unknown ();

  exit (0);
}
