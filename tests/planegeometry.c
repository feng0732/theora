/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggTheora SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE Theora SOURCE CODE IS COPYRIGHT (C) 2002-2009                *
 * by the Xiph.Org Foundation and contributors                      *
 * https://www.xiph.org/                                            *
 *                                                                  *
 ********************************************************************

  function: routines for validating that the encoded (padded) frame
            planes and the sampling grid always describe the same
            visible picture as the display crop rectangle

  Boundaries covered:
  1. Encoding sizes carrying block (16 pixel) margin: the luma and
     chroma active regions still map onto the display crop.
  2. Every chroma format, with varying crop offsets and row strides:
     edge padding never reaches the visible region.
  3. Decoded output keeps the true size and orientation of every
     plane, with failures attributed to cropping, sampling-grid, or
     stride handling.

 ********************************************************************/

#include <stdlib.h>
#include <string.h>
#include <theora/theoraenc.h>
#include <theora/theoradec.h>

#include "tests.h"

/*Sentinel value used to fill every byte the codec is not allowed to
   reference (padding and custom-stride gaps).*/
#define EDGE_SENTINEL 0xA5

/*A decoded picture can differ from the lossless source by a small amount
   of quantization noise; a sentinel leaking into the visible area would
   differ from the source by far more than this.*/
#define PIXEL_TOLERANCE 12

typedef struct{
  th_pixel_fmt pixel_fmt;
  int          frame_width;
  int          frame_height;
  int          pic_x;
  int          pic_y;
  int          pic_width;
  int          pic_height;
  int          stride_pad;
  int          bottom_up;
}plane_geometry_cfg;

/*Chrominance decimation factors for the X and Y directions, derived from
   the pixel format exactly the way the library does it:
   hdec = !(pixel_fmt&1), vdec = !(pixel_fmt&2).*/
static void
pf_decimation(th_pixel_fmt _pf,int *_hdec,int *_vdec){
  *_hdec=!(_pf&1);
  *_vdec=!(_pf&2);
}

static const char *
pf_name(th_pixel_fmt _pf){
  switch(_pf){
    case TH_PF_420:return "4:2:0";
    case TH_PF_422:return "4:2:2";
    case TH_PF_444:return "4:4:4";
    default:return "reserved";
  }
}

/*Fills in the expected geometry of one plane for a full frame buffer,
   matching the internal derivation in th_encode_ycbcr_in().*/
static void
expected_plane_geometry(const plane_geometry_cfg *_cfg,int _pli,
 int *_width,int *_height){
  int hdec;
  int vdec;
  pf_decimation(_cfg->pixel_fmt,&hdec,&vdec);
  if(_pli==0){
    *_width=_cfg->frame_width;
    *_height=_cfg->frame_height;
  }
  else{
    *_width=_cfg->frame_width>>hdec;
    *_height=_cfg->frame_height>>vdec;
  }
}

/*Deterministic, spatially varying sample value for a visible pixel.
   The pattern never takes the sentinel value and stays far from 0/255
   so low-pass padding extension is easy to distinguish from real data.*/
static unsigned char
visible_sample(int _pli,int _x,int _y){
  int v=(37+11*_pli+7*_x+5*_y+((_x^_y)<<1))&0x7F;
  if(v==EDGE_SENTINEL)v=(v+1)&0x7F;
  return (unsigned char)v;
}

/*Allocates a full-frame, three-plane buffer, fills every byte (including
   the stride gaps) with EDGE_SENTINEL, and then writes the visible crop
   region of each plane with the deterministic sample pattern.*/
static void
alloc_filled_frame(const plane_geometry_cfg *_cfg,th_ycbcr_buffer _buf,
 unsigned char **_storage){
  int hdec;
  int vdec;
  int pli;
  pf_decimation(_cfg->pixel_fmt,&hdec,&vdec);
  for(pli=0;pli<3;pli++){
    int width;
    int height;
    int stride;
    int x;
    int y;
    unsigned char *row;
    expected_plane_geometry(_cfg,pli,&width,&height);
    /*Give every plane a deliberately oversized row stride.*/
    stride=width+_cfg->stride_pad;
    if(_cfg->bottom_up)stride=-stride;
    _buf[pli].width=width;
    _buf[pli].height=height;
    _buf[pli].stride=stride;
    _buf[pli].data=NULL;
    *_storage=(unsigned char *)malloc(
     (size_t)abs(stride)*(size_t)height);
    if(*_storage==NULL)FAIL("out of memory allocating source plane");
    memset(*_storage,EDGE_SENTINEL,(size_t)abs(stride)*(size_t)height);
    _buf[pli].data=*_storage;
    if(_cfg->bottom_up){
      /*Point at the first displayed (top) row, which lives at the end of
         the bottom-up allocation.*/
      _buf[pli].data+=(height-1)*(ptrdiff_t)(-stride);
    }
    /*Paint only the visible crop region.
      Chroma crop coordinates follow the same shifts the encoder uses.*/
    if(pli==0){
      for(y=0;y<_cfg->pic_height;y++){
        row=_buf[pli].data+(_cfg->pic_y+y)*(ptrdiff_t)_buf[pli].stride;
        for(x=0;x<_cfg->pic_width;x++){
          row[_cfg->pic_x+x]=visible_sample(pli,_cfg->pic_x+x,
           _cfg->pic_y+y);
        }
      }
    }
    else{
      int cpic_x=_cfg->pic_x>>hdec;
      int cpic_y=_cfg->pic_y>>vdec;
      int cpic_width=((_cfg->pic_x+_cfg->pic_width+hdec)>>hdec)-cpic_x;
      int cpic_height=((_cfg->pic_y+_cfg->pic_height+vdec)>>vdec)-cpic_y;
      for(y=0;y<cpic_height;y++){
        row=_buf[pli].data+(cpic_y+y)*(ptrdiff_t)_buf[pli].stride;
        for(x=0;x<cpic_width;x++){
          row[cpic_x+x]=visible_sample(pli,cpic_x+x,cpic_y+y);
        }
      }
    }
    _storage++;
  }
}

static void
free_frame_storage(unsigned char *_storage[3]){
  int pli;
  for(pli=0;pli<3;pli++){
    free(_storage[pli]);
    _storage[pli]=NULL;
  }
}

/*A header packet whose bytes we own: the packets returned by
   th_encode_flushheader() reference library storage that is invalidated
   by the next encoder call, so every header is copied.*/
typedef struct{
  ogg_packet    op;
  unsigned char *data;
}owned_packet;

/*Builds an encoder and drains its three header packets into _hdr,
   copying the bytes so they stay valid until free_owned_headers().*/
static th_enc_ctx *
open_encoder(const th_info *_ti,owned_packet _hdr[3]){
  th_enc_ctx *te;
  th_comment  tc;
  int         ret;
  int         i;
  te=th_encode_alloc(_ti);
  if(te==NULL)FAIL("th_encode_alloc rejected a valid cropped geometry");
  th_comment_init(&tc);
  for(i=0;i<3;i++){
    ret=th_encode_flushheader(te,&tc,&_hdr[i].op);
    if(ret<=0){
      th_comment_clear(&tc);
      th_encode_free(te);
      FAIL("encoder failed to emit a header packet");
    }
    _hdr[i].data=(unsigned char *)malloc(_hdr[i].op.bytes);
    if(_hdr[i].data==NULL)FAIL("out of memory copying header packet");
    memcpy(_hdr[i].data,_hdr[i].op.packet,_hdr[i].op.bytes);
    _hdr[i].op.packet=_hdr[i].data;
  }
  th_comment_clear(&tc);
  return te;
}

static void
free_owned_headers(owned_packet _hdr[3]){
  int i;
  for(i=0;i<3;i++){
    free(_hdr[i].data);
    _hdr[i].data=NULL;
    _hdr[i].op.packet=NULL;
  }
}

/*Parses the three header packets and returns a ready decoder.
   Returns the parsed th_info separately so crop handling can be checked.*/
static th_dec_ctx *
open_decoder(const owned_packet _hdr[3],th_info *_dec_info,
 th_setup_info **_setup){
  th_comment tc;
  int        ret;
  int        i;
  th_info_init(_dec_info);
  th_comment_init(&tc);
  *_setup=NULL;
  for(i=0;i<3;i++){
    ogg_packet op=_hdr[i].op;
    ret=th_decode_headerin(_dec_info,&tc,_setup,&op);
    if(ret<0){
      th_comment_clear(&tc);
      FAIL("decoder rejected the headers produced by the encoder");
    }
  }
  th_comment_clear(&tc);
  return th_decode_alloc(_dec_info,*_setup);
}

/*Encodes exactly one frame and returns its single video packet.*/
static void
encode_one_frame(th_enc_ctx *_te,th_ycbcr_buffer _src,ogg_packet *_op){
  int ret;
  ret=th_encode_ycbcr_in(_te,_src);
  if(ret<0){
    printf("th_encode_ycbcr_in() returned %d\n",ret);
    FAIL("encoder refused a frame whose geometry matched th_info");
  }
  ret=th_encode_packetout(_te,1,_op);
  if(ret<=0)FAIL("encoder produced no video packet for the frame");
}

/*Expected full-frame plane dimensions for a given pixel format.*/
static void
expected_info_plane_geometry(const th_info *_ti,int _pli,
 int *_width,int *_height){
  int hdec;
  int vdec;
  pf_decimation(_ti->pixel_fmt,&hdec,&vdec);
  if(_pli==0){
    *_width=(int)_ti->frame_width;
    *_height=(int)_ti->frame_height;
  }
  else{
    *_width=(int)(_ti->frame_width>>hdec);
    *_height=(int)(_ti->frame_height>>vdec);
  }
}

/*Verifies that a decoded plane reports the real full-frame dimensions
   (not the cropped picture size) and a positive, top-down stride that is
   at least one byte per column.
   Any failure names the plane and the property at fault so that a broken
   build can be attributed to sampling-grid or stride handling.*/
static void
check_decoded_plane_geometry(const th_info *_ti,const th_ycbcr_buffer _buf,
 const char *_stage){
  int pli;
  for(pli=0;pli<3;pli++){
    int exp_width;
    int exp_height;
    expected_info_plane_geometry(_ti,pli,&exp_width,&exp_height);
    if(_buf[pli].width!=exp_width){
      printf("%s: plane %d width %d does not match the %ux%u sampling "
       "grid (expected %d)\n",_stage,pli,_buf[pli].width,
       _ti->frame_width,_ti->frame_height,exp_width);
      FAIL("decoded plane width lost its true sampling-grid size");
    }
    if(_buf[pli].height!=exp_height){
      printf("%s: plane %d height %d does not match the %ux%u sampling "
       "grid (expected %d)\n",_stage,pli,_buf[pli].height,
       _ti->frame_width,_ti->frame_height,exp_height);
      FAIL("decoded plane height lost its true sampling-grid size");
    }
    if(_buf[pli].stride<_buf[pli].width){
      printf("%s: plane %d stride %d is smaller than its width %d\n",
       _stage,pli,_buf[pli].stride,_buf[pli].width);
      FAIL("decoded plane stride cannot address every column");
    }
    if(_buf[pli].data==NULL)FAIL("decoder returned a null plane pointer");
  }
}

/*Compares the visible crop region of a decoded frame against the source
   pattern.  Padding outside the crop is deliberately ignored, matching
   the contract that only the crop is displayable.*/
static void
check_visible_pixels(const plane_geometry_cfg *_cfg,
 const th_ycbcr_buffer _dec,const char *_stage){
  int hdec;
  int vdec;
  int pli;
  pf_decimation(_cfg->pixel_fmt,&hdec,&vdec);
  for(pli=0;pli<3;pli++){
    int cpic_x;
    int cpic_y;
    int cpic_width;
    int cpic_height;
    int x;
    int y;
    if(pli==0){
      cpic_x=_cfg->pic_x;
      cpic_y=_cfg->pic_y;
      cpic_width=_cfg->pic_width;
      cpic_height=_cfg->pic_height;
    }
    else{
      cpic_x=_cfg->pic_x>>hdec;
      cpic_y=_cfg->pic_y>>vdec;
      cpic_width=((_cfg->pic_x+_cfg->pic_width+hdec)>>hdec)-cpic_x;
      cpic_height=((_cfg->pic_y+_cfg->pic_height+vdec)>>vdec)-cpic_y;
    }
    for(y=0;y<cpic_height;y++){
      const unsigned char *row=_dec[pli].data+
       (cpic_y+y)*(ptrdiff_t)_dec[pli].stride;
      for(x=0;x<cpic_width;x++){
        int expected=visible_sample(pli,cpic_x+x,cpic_y+y);
        int got=row[cpic_x+x];
        if(abs(got-expected)>PIXEL_TOLERANCE){
          printf("%s: plane %d visible pixel (%d,%d) = %d but the source "
           "crop sample was %d\n",_stage,pli,cpic_x+x,cpic_y+y,got,
           expected);
          FAIL("padding or stray data leaked into the visible crop region");
        }
      }
    }
  }
}

/*Runs one full encode/decode round trip for the given configuration and
   exercises every boundary in one place.*/
static void
round_trip(const plane_geometry_cfg *_cfg){
  th_info          ti;
  th_info          dec_info;
  th_setup_info   *setup;
  th_enc_ctx      *te;
  th_dec_ctx      *td;
  th_ycbcr_buffer  src;
  th_ycbcr_buffer  out;
  unsigned char   *storage[3];
  owned_packet     hdr[3];
  ogg_packet       op;
  int              hdec;
  int vdec;
  int pli;
  pf_decimation(_cfg->pixel_fmt,&hdec,&vdec);

  INFO ("+ Configuring th_info with a cropped picture");
  th_info_init(&ti);
  ti.frame_width=_cfg->frame_width;
  ti.frame_height=_cfg->frame_height;
  ti.pic_width=_cfg->pic_width;
  ti.pic_height=_cfg->pic_height;
  ti.pic_x=_cfg->pic_x;
  ti.pic_y=_cfg->pic_y;
  ti.fps_numerator=1;
  ti.fps_denominator=1;
  ti.aspect_numerator=1;
  ti.aspect_denominator=1;
  ti.colorspace=TH_CS_UNSPECIFIED;
  ti.pixel_fmt=_cfg->pixel_fmt;
  ti.quality=63;
  ti.target_bitrate=0;
  ti.keyframe_granule_shift=0;

  /*The crop rectangle must describe one visible picture inside the
     block-aligned encoded frame, for every plane including the
     subsampled chroma ones.*/
  if((ti.frame_width&0xF)||(ti.frame_height&0xF))
    FAIL("test bug: encoded frame dimensions must be multiples of 16");
  if(ti.pic_x+ti.pic_width>ti.frame_width||
   ti.pic_y+ti.pic_height>ti.frame_height)
    FAIL("test bug: display crop lies outside the encoded frame");
  if((ti.pic_x&(hdec))||(ti.pic_y&(vdec))||
   (ti.pic_width&(hdec))||(ti.pic_height&(vdec))){
    /*Odd crops with subsampled chroma are legal but discouraged; the
       configurations below keep even coordinates so the sampling grid
       stays unambiguous.*/
    WARN("odd crop with subsampled chroma relies on extended grid rules");
  }

  alloc_filled_frame(_cfg,src,storage);

  /*The application-facing source planes already have to expose the true
     frame-plane dimensions; the encoder must accept them as-is.*/
  for(pli=0;pli<3;pli++){
    int exp_width;
    int exp_height;
    expected_plane_geometry(_cfg,pli,&exp_width,&exp_height);
    if(src[pli].width!=exp_width||src[pli].height!=exp_height){
      FAIL("test bug: source plane does not match the sampling grid");
    }
  }

  te=open_encoder(&ti,hdr);
  td=open_decoder(hdr,&dec_info,&setup);
  if(td==NULL){
    th_setup_free(setup);
    th_encode_free(te);
    free_owned_headers(hdr);
    free_frame_storage(storage);
    FAIL("th_decode_alloc rejected a stream with valid crop parameters");
  }

  /*The crop parameters must survive the header round trip unchanged;
   otherwise the decoder would display the wrong visible rectangle.*/
  if(dec_info.frame_width!=ti.frame_width||
   dec_info.frame_height!=ti.frame_height||
   dec_info.pic_width!=ti.pic_width||
   dec_info.pic_height!=ti.pic_height||
   dec_info.pic_x!=ti.pic_x||dec_info.pic_y!=ti.pic_y||
   dec_info.pixel_fmt!=ti.pixel_fmt){
    FAIL("crop rectangle or pixel format was altered by header packing");
  }

  encode_one_frame(te,src,&op);
  if(th_decode_packetin(td,&op,NULL)<0)
    FAIL("decoder rejected the encoded frame packet");
  if(th_decode_ycbcr_out(td,out)<0)
    FAIL("decoder failed to produce an output frame");

  check_decoded_plane_geometry(&dec_info,out,"decode output");
  check_visible_pixels(_cfg,out,"decode output");

  th_decode_free(td);
  th_setup_free(setup);
  th_encode_free(te);
  free_frame_storage(storage);
  free_owned_headers(hdr);
  th_info_clear(&dec_info);
  th_info_clear(&ti);
}

/*Section 1: block-boundary margins.
   The encoded frame is padded out to 16 pixel boundaries while the
   displayed picture is smaller and offset; luma and chroma active
   regions must still line up with the crop.*/
static void
test_block_margin_crop(void){
  static const plane_geometry_cfg cfgs[]={
    /*Luma 48x48 frame, visible 40x32 picture inset on all sides.*/
    {TH_PF_420,48,48,4,8,40,32,8,0},
    /*A picture narrower than one macroblock, centered in a 32x32 frame.*/
    {TH_PF_420,32,32,8,8,16,16,16,0},
    /*Crop flush against the top-left corner, margin only on the right
       and bottom.*/
    {TH_PF_420,64,64,0,0,52,44,4,0},
    /*Crop flush against the bottom-right corner.*/
    {TH_PF_420,64,32,4,0,60,32,12,0},
    /*The full frame is itself visible: zero crop margin must still work.*/
    {TH_PF_420,32,16,0,0,32,16,8,0}
  };
  size_t i;
  for(i=0;i<sizeof(cfgs)/sizeof(cfgs[0]);i++){
    printf("----  block-margin case %u: frame %ux%u crop +%u,+%u %ux%u\n",
     (unsigned)i,cfgs[i].frame_width,cfgs[i].frame_height,
     cfgs[i].pic_x,cfgs[i].pic_y,cfgs[i].pic_width,cfgs[i].pic_height);
    round_trip(cfgs+i);
  }
}

/*Section 2: every chroma format, varying offsets, strides and buffer
   orientation.  The padding bytes are filled with a sentinel, so any
   read of the margins by the encoder or any leak into the decoded
   visible region is caught by the pixel comparison.*/
static void
test_chroma_formats_and_strides(void){
  static const plane_geometry_cfg cfgs[]={
    /*4:2:0, even crop, padded positive stride.*/
    {TH_PF_420,48,48,8,8,32,24,8,0},
    /*4:2:0, same geometry but a bottom-up (negative stride) source.*/
    {TH_PF_420,48,48,8,8,32,24,8,1},
    /*4:2:2: horizontal-only chroma decimation, full chroma height.*/
    {TH_PF_422,48,32,8,4,32,24,8,0},
    /*4:2:2 with a wide, custom stride and an offset crop.*/
    {TH_PF_422,64,48,16,8,32,32,32,0},
    /*4:2:2 bottom-up.*/
    {TH_PF_422,48,32,8,4,32,24,8,1},
    /*4:4:4: no decimation; chroma planes equal the luma plane.*/
    {TH_PF_444,32,32,4,4,24,24,8,0},
    /*4:4:4 with a large stride gap and asymmetric margins.*/
    {TH_PF_444,64,48,16,8,40,32,48,0},
    /*4:4:4 bottom-up.*/
    {TH_PF_444,32,32,4,4,24,24,8,1}
  };
  size_t i;
  for(i=0;i<sizeof(cfgs)/sizeof(cfgs[0]);i++){
    printf("----  chroma/stride case %u: %s frame %ux%u crop +%u,+%u "
     "%ux%u stride_pad %d%s\n",(unsigned)i,pf_name(cfgs[i].pixel_fmt),
     cfgs[i].frame_width,cfgs[i].frame_height,cfgs[i].pic_x,
     cfgs[i].pic_y,cfgs[i].pic_width,cfgs[i].pic_height,
     cfgs[i].stride_pad,cfgs[i].bottom_up?" bottom-up":" top-down");
    round_trip(cfgs+i);
  }
}

/*Section 3: decoded plane geometry and orientation.
   Beyond the round trips above, this checks the picture-sized input
   mode (where the encoder repositions the buffer onto the full frame
   using only the crop offsets) and asserts that decoder-visible plane
   metadata always reflects the full frame, never the crop.*/
static void
test_decoded_plane_geometry(void){
  static const plane_geometry_cfg cfgs[]={
    {TH_PF_420,48,48,8,8,28,20,0,0},
    {TH_PF_422,48,32,8,4,30,24,0,0},
    {TH_PF_444,32,32,4,4,24,24,0,0}
  };
  size_t i;
  for(i=0;i<sizeof(cfgs)/sizeof(cfgs[0]);i++){
    const plane_geometry_cfg *cfg=cfgs+i;
    th_info          ti;
    th_info          dec_info;
    th_setup_info   *setup;
    th_enc_ctx      *te;
    th_dec_ctx      *td;
    th_ycbcr_buffer  pic;
    th_ycbcr_buffer  out;
    unsigned char   *storage[3];
    owned_packet     hdr[3];
    ogg_packet       op;
    int              hdec;
    int vdec;
    int pli;
    pf_decimation(cfg->pixel_fmt,&hdec,&vdec);
    printf("----  picture-sized input case %u: %s frame %ux%u crop "
     "+%u,+%u %ux%u\n",(unsigned)i,pf_name(cfg->pixel_fmt),
     cfg->frame_width,cfg->frame_height,cfg->pic_x,cfg->pic_y,
     cfg->pic_width,cfg->pic_height);

    th_info_init(&ti);
    ti.frame_width=cfg->frame_width;
    ti.frame_height=cfg->frame_height;
    ti.pic_width=cfg->pic_width;
    ti.pic_height=cfg->pic_height;
    ti.pic_x=cfg->pic_x;
    ti.pic_y=cfg->pic_y;
    ti.fps_numerator=1;
    ti.fps_denominator=1;
    ti.aspect_numerator=1;
    ti.aspect_denominator=1;
    ti.colorspace=TH_CS_UNSPECIFIED;
    ti.pixel_fmt=cfg->pixel_fmt;
    ti.quality=63;
    ti.target_bitrate=0;
    ti.keyframe_granule_shift=0;

    /*Allocate buffers sized to the picture region only.  The encoder is
       documented to rebase these onto a full frame via the crop
       offsets, which exercises pic_x/pic_y pointer arithmetic.*/
    for(pli=0;pli<3;pli++){
      int cpic_width;
      int cpic_height;
      int x;
      int y;
      unsigned char *row;
      if(pli==0){
        cpic_width=cfg->pic_width;
        cpic_height=cfg->pic_height;
      }
      else{
        cpic_width=((cfg->pic_x+cfg->pic_width+hdec)>>hdec)
         -(cfg->pic_x>>hdec);
        cpic_height=((cfg->pic_y+cfg->pic_height+vdec)>>vdec)
         -(cfg->pic_y>>vdec);
      }
      pic[pli].width=cpic_width;
      pic[pli].height=cpic_height;
      /*Tight stride, distinct from any full-frame stride.*/
      pic[pli].stride=cpic_width;
      storage[pli]=(unsigned char *)malloc(
       (size_t)cpic_width*(size_t)cpic_height);
      if(storage[pli]==NULL)FAIL("out of memory allocating picture plane");
      pic[pli].data=storage[pli];
      for(y=0;y<cpic_height;y++){
        row=pic[pli].data+y*(ptrdiff_t)pic[pli].stride;
        for(x=0;x<cpic_width;x++){
          /*Picture-local coordinates map back to crop coordinates in
             frame space.*/
          int gx=(pli==0?cfg->pic_x:cfg->pic_x>>hdec)+x;
          int gy=(pli==0?cfg->pic_y:cfg->pic_y>>vdec)+y;
          row[x]=visible_sample(pli,gx,gy);
        }
      }
    }

    te=open_encoder(&ti,hdr);
    td=open_decoder(hdr,&dec_info,&setup);
    if(td==NULL){
      th_setup_free(setup);
      th_encode_free(te);
      free_owned_headers(hdr);
      FAIL("th_decode_alloc rejected a valid picture-sized stream");
    }
    encode_one_frame(te,pic,&op);
    if(th_decode_packetin(td,&op,NULL)<0)
      FAIL("decoder rejected the frame encoded from a picture-sized buffer");
    if(th_decode_ycbcr_out(td,out)<0)
      FAIL("decoder produced no output for picture-sized input");

    /*Output planes must still describe the full encoded frame, with
       positive top-down strides, even though the input only carried the
       crop.  This is where confusing crop size with plane size would
       surface.*/
    check_decoded_plane_geometry(&dec_info,out,"picture-sized decode");
    if(out[0].stride<=0||out[1].stride<=0||out[2].stride<=0)
      FAIL("decoder did not return top-down planes with positive strides");
    check_visible_pixels(cfg,out,"picture-sized decode");

    th_decode_free(td);
    th_setup_free(setup);
    th_encode_free(te);
    free_frame_storage(storage);
    free_owned_headers(hdr);
    th_info_clear(&dec_info);
    th_info_clear(&ti);
  }
}

int
main(int argc,char *argv[]){
  (void)argc;(void)argv;
  test_block_margin_crop();
  test_chroma_formats_and_strides();
  test_decoded_plane_geometry();
  exit(0);
}
