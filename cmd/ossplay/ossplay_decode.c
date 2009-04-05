/*
 * Purpose: Sample format decode routines for ossplay
 *
 */
#define COPYING Copyright (C) Hannu Savolainen and Dev Mazumdar 2000-2008. All rights reserved.

#include "ossplay_decode.h"
#include "ossplay_wparser.h"

typedef struct cradpcm_values {
  const unsigned char * const * table;

  signed char limit;
  signed char shift;
  signed char step;
  unsigned char ratio;
  unsigned char pred;
}
cradpcm_values_t;

typedef struct fib_values {
  unsigned char pred;
  const signed char * table;
}
fib_values_t;

extern int amplification, force_speed, force_fmt, force_channels;
extern flag int_conv, overwrite, verbose;
extern char audio_devname[32];
extern off_t (*ossplay_lseek) (int, off_t, int);
extern double seek_time;

static void decode_ima (unsigned char *, unsigned char *, ssize_t, int16 *,
                        int8 *, int, int);
static void decode_ima_3bits (unsigned char *, unsigned char *, ssize_t,
                              int16 *, int8 *, int, int);
static decfunc_t decode_24;
static decfunc_t decode_8_to_s16;
static decfunc_t decode_amplify;
static decfunc_t decode_cr;
static decfunc_t decode_double64_be;
static decfunc_t decode_double64_le;
static decfunc_t decode_endian;
static decfunc_t decode_fib;
static decfunc_t decode_float32_be;
static decfunc_t decode_float32_le;
static decfunc_t decode_mac_ima;
static decfunc_t decode_mono_to_stereo;
static decfunc_t decode_ms_ima;
static decfunc_t decode_ms_adpcm; 
static decfunc_t decode_nul;

static int32 float32_to_s32 (int, int, int);
static int32 double64_to_s32 (int, int32, int32, int);

static cradpcm_values_t * setup_cr (int, int);
static fib_values_t * setup_fib (int, int);
static decoders_queue_t * setup_normalize (int *, int *, decoders_queue_t *);

static seekfunc_t seek_normal;
static seekfunc_t seek_compressed;

errors_t
decode_sound (dspdev_t * dsp, int fd, unsigned long long filesize, int format,
              int channels, int speed, void * metadata)
{
  decoders_queue_t * dec, * decoders;
  seekfunc_t * seekf = NULL;
  int bsize, obsize;
  double constant;
  errors_t ret = E_DECODE;

  if (force_speed != 0) speed = force_speed;
  if (force_channels != 0) channels = force_channels;
  if (force_fmt != 0) format = force_fmt;
  if ((channels > MAX_CHANNELS) || (channels == 0))
    {
      print_msg (ERRORM, "An unreasonable number of channels (%d), aborting\n",
                 channels);
      return E_DECODE;
    }

  constant = format2bits (format) * speed * channels / 8.0;
#if 0
  /*
   * There is no reason to use SNDCTL_DSP_GETBLKSIZE in applications like this.
   * Using some fixed local buffer size will work equally well.
   */
  ioctl (dsp->fd, SNDCTL_DSP_GETBLKSIZE, &bsize);
#else
  bsize = PLAYBUF_SIZE;
#endif

  if (filesize < 2) return 0;
  decoders = dec = ossplay_malloc (sizeof (decoders_queue_t));
  dec->next = NULL;
  dec->flag = 0;
  seekf = seek_normal;

  switch (format)
    {
      case AFMT_MS_ADPCM:
        if (metadata == NULL)
          {
            msadpcm_values_t * val = ossplay_malloc (sizeof (msadpcm_values_t));

            val->nBlockAlign = 256; val->wSamplesPerBlock = 496;
            val->wNumCoeff = 7; val->channels = DEFAULT_CHANNELS;
            val->coeff[0].coeff1 = 256; val->coeff[0].coeff2 = 0;
            val->coeff[1].coeff1 = 512; val->coeff[1].coeff2 = -256;
            val->coeff[2].coeff1 = 0; val->coeff[2].coeff2 = 0;
            val->coeff[3].coeff1 = 192; val->coeff[3].coeff2 = 64;
            val->coeff[4].coeff1 = 240; val->coeff[4].coeff2 = 0;
            val->coeff[5].coeff1 = 460; val->coeff[5].coeff2 = -208;
            val->coeff[6].coeff1 = 392; val->coeff[6].coeff2 = -232;

            dec->metadata = (void *)val;
            dec->flag |= FREE_META;
          }
        else dec->metadata = metadata;
        dec->decoder = decode_ms_adpcm;
        bsize = ((msadpcm_values_t *)dec->metadata)->nBlockAlign;
        if (bsize == 0) /* Let's try anyway */
          {
            bsize = filesize - filesize % 4;
          }
        if ((bsize > 16*1024*1024) || (bsize == 0))
          {
            print_msg (ERRORM,
                       "Unreasonable nBlockAlign (%d), aborting\n", bsize);
            goto exit;
          }
        obsize = 4 * bsize;
        dec->outbuf = ossplay_malloc (obsize);
        dec->flag |= FREE_OBUF;
        seekf = seek_compressed;

        format = AFMT_S16_LE;
        break;
      case AFMT_MS_IMA_ADPCM:
      case AFMT_MS_IMA_ADPCM_3BITS:
        dec->metadata = metadata;
        if (dec->metadata == NULL) goto exit;
        dec->decoder = decode_ms_ima;
        bsize = ((msadpcm_values_t *)dec->metadata)->nBlockAlign;
        if (bsize == 0) /* Let's try anyway */
          {
            bsize = filesize - filesize % 4;
          }
        if ((bsize > 16*1024*1024) || (bsize == 0))
          {
            print_msg (ERRORM,
                       "Unreasonable nBlockAlign (%d), aborting\n", bsize);
            goto exit;
          }
        if (format == AFMT_MS_IMA_ADPCM_3BITS)
          obsize = (bsize * 16)/3 + 2;
         /*
          * 8 sample words per 3 bytes, each expanding to 2 bytes, plus 2 bytes
          * to deal with fractions. Slight overestimation because bsize
          * includes the headers too. 
          */
        else
          obsize = 4 * bsize;
        dec->outbuf = ossplay_malloc (obsize);
        dec->flag = FREE_OBUF;
        seekf = seek_compressed;

        format = AFMT_S16_NE;
        break;
      case AFMT_MAC_IMA_ADPCM:
        dec->metadata = (void *)(long)channels;
        dec->decoder = decode_mac_ima;
        bsize = PLAYBUF_SIZE - PLAYBUF_SIZE % (MAC_IMA_BLKLEN * channels);
        obsize = 4 * bsize;
        dec->outbuf = ossplay_malloc (obsize);
        dec->flag = FREE_OBUF;
        seekf = seek_compressed;

        format = AFMT_S16_NE;
        break;
      case AFMT_CR_ADPCM_2:
      case AFMT_CR_ADPCM_3:
      case AFMT_CR_ADPCM_4:
        dec->metadata = (void *)setup_cr (fd, format);;
        if (dec->metadata == NULL) goto exit;
        dec->decoder = decode_cr;
        obsize = ((cradpcm_values_t *)dec->metadata)->ratio * bsize;
        dec->outbuf = ossplay_malloc (obsize);
        dec->flag = FREE_OBUF | FREE_META;
        seekf = seek_compressed;

        if (filesize != UINT_MAX) filesize--;
        format = AFMT_U8;
        break;
      case AFMT_FIBO_DELTA:
      case AFMT_EXP_DELTA:
        dec->metadata = (void *)setup_fib (fd, format);;
        if (dec->metadata == NULL) goto exit;
        dec->decoder = decode_fib;
        obsize = 2 * bsize;
        dec->outbuf = ossplay_malloc (obsize);
        dec->flag = FREE_OBUF | FREE_META;
        seekf = seek_compressed;

        if (filesize != UINT_MAX) filesize--;
        format = AFMT_U8;
        break;
      case AFMT_S24_PACKED:
      case AFMT_S24_PACKED_BE:
        dec->metadata = (void *)(long)format;
        dec->decoder = decode_24;
        bsize = PLAYBUF_SIZE - PLAYBUF_SIZE % 3;
        obsize = bsize/3*4;
        dec->outbuf = ossplay_malloc (obsize);
        dec->flag = FREE_OBUF;

        format = AFMT_S32_NE;
        break;
      case AFMT_FLOAT32_BE:
      case AFMT_FLOAT32_LE:
        if (format == AFMT_FLOAT32_BE) dec->decoder = decode_float32_be;
        else dec->decoder = decode_float32_le;
        bsize = PLAYBUF_SIZE;
        obsize = bsize;
        dec->outbuf = NULL;

        format = AFMT_S32_NE;
        break;
      case AFMT_DOUBLE64_BE:
      case AFMT_DOUBLE64_LE:
        if (format == AFMT_DOUBLE64_BE) dec->decoder = decode_double64_be;
        else dec->decoder = decode_double64_le;
        bsize = PLAYBUF_SIZE - PLAYBUF_SIZE % 2;
        obsize = bsize/2;
        dec->outbuf = NULL;

        format = AFMT_S32_NE;
        break;
      default:
        dec->decoder = decode_nul;

        obsize = bsize;
        break;
    }

  if (int_conv)
    decoders = setup_normalize (&format, &obsize, decoders);

  if ((amplification > 0) && (amplification != 100))
    {
      decoders->next = ossplay_malloc (sizeof (decoders_queue_t));
      decoders = decoders->next;
      decoders->metadata = (void *)(long)format;
      decoders->decoder = decode_amplify;
      decoders->next = NULL;
      decoders->outbuf = NULL;
      decoders->flag = 0;
    }

  ret = setup_device (dsp, format, channels, speed);
  if (ret == E_FORMAT_UNSUPPORTED)
    {
      int i, tmp;

      for (i = 0; format_a[i].name != NULL; i++)
        if (format_a[i].fmt == format)
          {
            tmp = format_a[i].may_conv;
            if ((tmp == 0) || (tmp == format)) continue;
            print_msg (WARNM, "Converting to format %s\n",
                       sample_format_name (tmp));
            ret = setup_device (dsp, tmp, channels, speed);
            decoders = setup_normalize (&format, &obsize, decoders);
            goto dcont;
          }
      goto exit;
    }

dcont:
  if ((ret == E_CHANNELS_UNSUPPORTED) && (channels == 1))
    {
      channels = 2;
      if ((ret = setup_device (dsp, format, channels, speed))) goto exit;
      decoders->next = ossplay_malloc (sizeof (decoders_queue_t));
      decoders = decoders->next;
      decoders->metadata = (void *)(long)format;
      decoders->decoder = decode_mono_to_stereo;
      decoders->next = NULL;
      obsize *= 2;
      decoders->outbuf = ossplay_malloc (obsize);
      decoders->flag = FREE_OBUF;
    }

  if (ret) goto exit;

  ret = play (dsp, fd, &filesize, bsize, constant, dec, seekf);

exit:
  decoders = dec;
  while (decoders != NULL)
    {
      if (decoders->flag & FREE_META) ossplay_free (decoders->metadata);
      if (decoders->flag & FREE_OBUF) ossplay_free (decoders->outbuf);
      decoders = decoders->next;
      ossplay_free (dec);
      dec = decoders;
    }

  return ret;
}

errors_t
encode_sound (dspdev_t * dsp, fctypes_t type, const char * fname, int format,
              int channels, int speed, unsigned long long datalimit)
{
  unsigned long long datasize = 0;
  double constant;
  int fd = -1;
  decoders_queue_t * dec, * decoders = NULL;
  errors_t ret;
  FILE * wave_fp;

  if ((ret = setup_device (dsp, format, channels, speed))) return ret;
  constant = format2bits (format) * speed * channels / 8.0;

  if (datalimit != 0) datalimit *= constant;

  if (strcmp (fname, "-") == 0)
    wave_fp = fdopen (1, "wb");
  else
    {
      fd = open (fname, O_WRONLY | O_CREAT | (overwrite?0:O_EXCL), 0644);
      if (fd == -1)
        {
          perror (fname);
          return E_ENCODE;
        }
      wave_fp = fdopen (fd, "wb");
    }

  if (wave_fp == NULL)
    {
      perror (fname);
      if (fd != -1) close (fd);
      return E_ENCODE;
    }

  if (channels == 1)
    print_msg (VERBOSEM, "Recording wav: Speed %dHz %d bits Mono\n",
               speed, (int)format2bits (format));
  if (channels == 2)
    print_msg (VERBOSEM, "Recording wav: Speed %dHz %d bits Stereo\n",
               speed, (int)format2bits (format));
  if (channels > 2)
    print_msg (VERBOSEM, "Recording wav: Speed %dHz %d bits %d channels\n",
               speed, (int)format2bits (format), channels);

  /* 
   * Write the initial header
   */
  if (write_head (wave_fp, type, datalimit, format, channels, speed) == -1)
    return E_ENCODE;

  decoders = dec = ossplay_malloc (sizeof (decoders_queue_t));
  dec->next = NULL;
  dec->flag = 0;
  dec->decoder = decode_nul;

  if ((amplification > 0) && (amplification != 100))
    {
      decoders->next = ossplay_malloc (sizeof (decoders_queue_t));
      decoders = decoders->next;
      decoders->metadata = (void *)(long)format;
      decoders->decoder = decode_amplify;
      decoders->next = NULL;
      decoders->outbuf = NULL;
      decoders->flag = 0;
    }

  ret = record (dsp, wave_fp, fname, constant, datalimit, &datasize, dec);

  finalize_head (wave_fp, type, datasize, format, channels, speed);
  if (fclose (wave_fp) != 0)
    {
      perror (fname);
      ret = E_ENCODE;
    }

  decoders = dec;
  while (decoders != NULL)
    {
      if (decoders->flag & FREE_META) ossplay_free (decoders->metadata);
      if (decoders->flag & FREE_OBUF) ossplay_free (decoders->outbuf);
      decoders = decoders->next;
      ossplay_free (dec);
      dec = decoders;
    }
  return ret;
}

static ssize_t
decode_24 (unsigned char ** obuf, unsigned char * buf,
           ssize_t l, void * metadata)
{
  unsigned long long outlen = 0;
  ssize_t i;
  int v1;
  uint32 * u32;
  int32 sample_s32, * outbuf = (int32 *) * obuf;;
  int format = (int)(long)metadata;

  if (format == AFMT_S24_PACKED) v1 = 8;
  else v1 = 24;

  for (i = 0; i < l-2; i += 3)
    {
      u32 = (uint32 *) &sample_s32;	/* Alias */

      *u32 = (buf[i] << v1) | (buf[i + 1] << 16) | (buf[i + 2] << (32-v1));
      outbuf[outlen++] = sample_s32;
    }

  return 4 * outlen;
}

static fib_values_t *
setup_fib (int fd, int format)
{
  static const signed char CodeToDelta[16] = {
    -34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21
  };
  static const signed char CodeToExpDelta[16] = {
    -128, -64, -32, -16, -8, -4, -2, -1, 0, 1, 2, 4, 8, 16, 32, 64
  };
  unsigned char buf;
  fib_values_t * val;

  val = ossplay_malloc (sizeof (fib_values_t));
  if (format == AFMT_EXP_DELTA) val->table = CodeToExpDelta;
  else val->table = CodeToDelta;

  if (read (fd, &buf, 1) <= 0) return NULL;
  val->pred = buf;

  return val;
}

static cradpcm_values_t *
setup_cr (int fd, int format)
{
  static const unsigned char T2[4][3] = {
    { 128, 6, 1 },
    { 32,  4, 1 },
    { 8,   2, 1 },
    { 2,   0, 1 }
  };

  static const unsigned char T3[3][3] = {
    { 128, 5, 3 },
    { 16,  2, 3 },
    { 2,   0, 1 }
  };

  static const unsigned char T4[2][3] = {
    { 128, 4, 7 },
    { 8,   0, 7 }
  };

  static const unsigned char * t_row[4];

  unsigned char buf;
  cradpcm_values_t * val;
  int i;

  val = ossplay_malloc (sizeof (cradpcm_values_t));
  val->table = t_row;

  if (format == AFMT_CR_ADPCM_2)
    {
      val->limit = 1;
      val->step = val->shift = 2;
      val->ratio = 4;
      for (i=0; i < 4; i++) t_row[i] = T2[i];
    }
  else if (format == AFMT_CR_ADPCM_3)
    {
      val->limit = 3;
      val->ratio = 3;
      val->step = val->shift = 0;
      for (i=0; i < 3; i++) t_row[i] = T3[i];
    }
  else /* if (format == AFMT_CR_ADPCM_4) */
    {
      val->limit = 5;
      val->ratio = 2;
      val->step = val->shift = 0;
      for (i=0; i < 2; i++) t_row[i] = T4[i];
    }

  if (read (fd, &buf, 1) <= 0) return NULL;
  val->pred = buf;

  return val;
}

static ssize_t
decode_8_to_s16 (unsigned char ** obuf, unsigned char * buf,
                 ssize_t l, void * metadata)
{
  int format = (int)(long)metadata;
  ssize_t i;
  int16 * outbuf = (int16 *) * obuf;
  static const int16 mu_law_table[256] = { 
    -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956, 
    -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764, 
    -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412, 
    -11900,-11388,-10876,-10364,-9852, -9340, -8828, -8316, 
    -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140, 
    -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092, 
    -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004, 
    -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980, 
    -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436, 
    -1372, -1308, -1244, -1180, -1116, -1052, -988,  -924, 
    -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652, 
    -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396, 
    -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260, 
    -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132, 
    -120,  -112,  -104,  -96,   -88,   -80,   -72,   -64, 
    -56,   -48,   -40,   -32,   -24,   -16,   -8,     0, 
    32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956, 
    23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764, 
    15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412, 
    11900, 11388, 10876, 10364, 9852,  9340,  8828,  8316, 
    7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140, 
    5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092, 
    3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004, 
    2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980, 
    1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436, 
    1372,  1308,  1244,  1180,  1116,  1052,  988,   924, 
    876,   844,   812,   780,   748,   716,   684,   652, 
    620,   588,   556,   524,   492,   460,   428,   396, 
    372,   356,   340,   324,   308,   292,   276,   260, 
    244,   228,   212,   196,   180,   164,   148,   132, 
    120,   112,   104,   96,    88,    80,    72,    64, 
    56,    48,    40,    32,    24,    16,    8,     0 
  };

  static const int16 a_law_table[256] = { 
    -5504, -5248, -6016, -5760, -4480, -4224, -4992, -4736, 
    -7552, -7296, -8064, -7808, -6528, -6272, -7040, -6784, 
    -2752, -2624, -3008, -2880, -2240, -2112, -2496, -2368, 
    -3776, -3648, -4032, -3904, -3264, -3136, -3520, -3392, 
    -22016,-20992,-24064,-23040,-17920,-16896,-19968,-18944, 
    -30208,-29184,-32256,-31232,-26112,-25088,-28160,-27136, 
    -11008,-10496,-12032,-11520,-8960, -8448, -9984, -9472, 
    -15104,-14592,-16128,-15616,-13056,-12544,-14080,-13568, 
    -344,  -328,  -376,  -360,  -280,  -264,  -312,  -296, 
    -472,  -456,  -504,  -488,  -408,  -392,  -440,  -424, 
    -88,   -72,   -120,  -104,  -24,   -8,    -56,   -40, 
    -216,  -200,  -248,  -232,  -152,  -136,  -184,  -168, 
    -1376, -1312, -1504, -1440, -1120, -1056, -1248, -1184, 
    -1888, -1824, -2016, -1952, -1632, -1568, -1760, -1696, 
    -688,  -656,  -752,  -720,  -560,  -528,  -624,  -592, 
    -944,  -912,  -1008, -976,  -816,  -784,  -880,  -848, 
    5504,  5248,  6016,  5760,  4480,  4224,  4992,  4736, 
    7552,  7296,  8064,  7808,  6528,  6272,  7040,  6784, 
    2752,  2624,  3008,  2880,  2240,  2112,  2496,  2368, 
    3776,  3648,  4032,  3904,  3264,  3136,  3520,  3392, 
    22016, 20992, 24064, 23040, 17920, 16896, 19968, 18944, 
    30208, 29184, 32256, 31232, 26112, 25088, 28160, 27136, 
    11008, 10496, 12032, 11520, 8960,  8448,  9984,  9472, 
    15104, 14592, 16128, 15616, 13056, 12544, 14080, 13568, 
    344,   328,   376,   360,   280,   264,   312,   296, 
    472,   456,   504,   488,   408,   392,   440,   424, 
    88,    72,    120,   104,   24,    8,     56,    40, 
    216,   200,   248,   232,   152,   136,   184,   168, 
    1376,  1312,  1504,  1440,  1120,  1056,  1248,  1184, 
    1888,  1824,  2016,  1952,  1632,  1568,  1760,  1696, 
    688,   656,   752,   720,   560,   528,   624,   592, 
    944,   912,   1008,  976,   816,   784,   880,   848 
  };

  switch (format)
    {
      case AFMT_U8:
        for (i = 0; i < l; i++) outbuf[i] = (buf[i] - 128) << 8;
        break;
      case AFMT_S8:
        for (i = 0; i < l; i++) outbuf[i] = buf[i] << 8;
        break;
      case AFMT_MU_LAW:
        for (i = 0; i < l; i++) outbuf[i] = mu_law_table[buf[i]];
        break;
      case AFMT_A_LAW:
        for (i = 0; i < l; i++) outbuf[i] = a_law_table[buf[i]];
        break;
    }

  return 2*l;
}

static ssize_t
decode_cr (unsigned char ** obuf, unsigned char * buf,
           ssize_t l, void * metadata)
{
  cradpcm_values_t * val = (cradpcm_values_t *) metadata;
  int j, pred = val->pred, step = val->step;
  unsigned char value;
  signed char sign;
  ssize_t i;

  for (i=0; i < l; i++)
    for (j=0; j < val->ratio; j++)
      {
        sign = (buf[i] & val->table[j][0])?-1:1;
        value = (buf[i] >> val->table[j][1]) & val->table[j][2];
        pred += sign*(value << step);
        if (pred > 255) pred = 255;
        else if (pred < 0) pred = 0;
        (*obuf)[val->ratio*i+j] = pred;
        if ((value >= val->limit) && (step < 3+val->shift)) step++;
        if ((value == 0) && (step > val->shift)) step--;
      }

  val->pred = pred;
  val->step = step;
  return val->ratio*l;
}

static ssize_t
decode_fib (unsigned char ** obuf, unsigned char * buf,
            ssize_t l, void * metadata)
{
  fib_values_t * val = (fib_values_t *)metadata;
  int x = val->pred;
  unsigned char d;
  ssize_t i;

  for (i = 0; i < 2*l; i++)
    {
      d = buf[i/2];
      if (i & 1) d &= 0xF;
      else d >>= 4;
      x += val->table[d];
      if (x > 255) x = 255;
      if (x < 0) x = 0;
      (*obuf)[i] = x;
    }

  val->pred = x;
  return 2*l;
}

static ssize_t
decode_ms_adpcm (unsigned char ** obuf, unsigned char * buf,
                 ssize_t l, void * metadata)
{
  msadpcm_values_t * val = (msadpcm_values_t *)metadata;

  int error_delta, i_delta, i = 0, nib = 0, channels = val->channels;
  int AdaptionTable[16] = {
    230, 230, 230, 230, 307, 409, 512, 614,
    768, 614, 512, 409, 307, 230, 230, 230
  };
  int32 delta[MAX_CHANNELS], samp1[MAX_CHANNELS], samp2[MAX_CHANNELS],
        predictor[MAX_CHANNELS], new, pred, n = 0;
  ssize_t outp = 0, x = 0;

/*
 * Playback procedure
 */
#define OUT_SAMPLE(s) \
 do { \
   if (s > 32767) s = 32767; else if (s < -32768) s = -32768; \
   (*obuf)[outp++] = (uint8)(s & 0xff); \
   (*obuf)[outp++] = (uint8)((s >> 8) & 0xff); \
   n += 2; \
 } while(0)

#define GETNIBBLE \
        ((nib == 0) ? \
                (buf[x + nib++] >> 4) & 0x0f : \
                buf[x++ + --nib] & 0x0f \
	)

  for (i = 0; i < channels; i++)
    {
      predictor[i] = buf[x];
      x++;
    }

  for (i = 0; i < channels; i++)
    {
      delta[i] = (int16) le_int (&buf[x], 2);
      x += 2;
    }

  for (i = 0; i < channels; i++)
    {
      samp1[i] = (int16) le_int (&buf[x], 2);
      x += 2;
      OUT_SAMPLE (samp2[i]);
    }

  for (i = 0; i < channels; i++)
    {
      samp2[i] = (int16) le_int (&buf[x], 2);
      x += 2;
      OUT_SAMPLE (samp2[i]);
    }

  while (n < (val->wSamplesPerBlock * 2 * channels))
    for (i = 0; i < channels; i++)
      {
        pred = ((samp1[i] * val->coeff[predictor[i]].coeff1)
                + (samp2[i] * val->coeff[predictor[i]].coeff2)) / 256;

        if (x > l) return outp;
        i_delta = error_delta = GETNIBBLE;

        if (i_delta & 0x08)
        i_delta -= 0x10;	/* Convert to signed */

        new = pred + (delta[i] * i_delta);
        OUT_SAMPLE (new);

        delta[i] = delta[i] * AdaptionTable[error_delta] / 256;
        if (delta[i] < 16) delta[i] = 16;

        samp2[i] = samp1[i];
        samp1[i] = new;
      }

  return outp;
}

static ssize_t
decode_nul (unsigned char ** obuf, unsigned char * buf,
            ssize_t l, void * metadata)
{
  *obuf = buf;
  return l;
}

static ssize_t
decode_endian (unsigned char ** obuf, unsigned char * buf,
               ssize_t l, void * metadata)
{
  int format = (int)(long)metadata;
  ssize_t i;

  switch (format)
    {
      case AFMT_S16_OE:
        {
          short * s = (short *)buf;

          for (i = 0; i < l / 2; i++)
            s[i] = ((s[i] >> 8) & 0x00FF) |
                   ((s[i] << 8) & 0xFF00);
        }
        break;
      case AFMT_S32_OE:
      case AFMT_S24_OE:
        {
          int * s = (int *)buf;

          for (i = 0; i < l / 4; i++)
            s[i] = ((s[i] >> 24) & 0x000000FF) |
                   ((s[i] << 8) & 0x00FF0000) | ((s[i] >> 8) & 0x0000FF00) |
                   ((s[i] << 24) & 0xFF000000);
        }
        break;
#ifdef OSS_LITTLE_ENDIAN
      case AFMT_U16_BE: /* U16_BE -> S16_LE */
#else
      case AFMT_U16_LE: /* U16_LE -> S16_BE */
#endif
        {
          short * s = (short *)buf;

          for (i = 0; i < l / 2; i++)
            s[i] = (((s[i] >> 8) & 0x00FF) | ((s[i] << 8) & 0xFF00)) -
                   USHRT_MAX/2;
        }
      break;
 /* Not an endian conversion, but included for completeness sake */
#ifdef OSS_LITTLE_ENDIAN
      case AFMT_U16_LE: /* U16_LE -> S16_LE */
#else
      case AFMT_U16_BE: /* U16_BE -> S16_BE */ 
#endif
       {
          short * s = (short *)buf;

          for (i = 0; i < l / 2; i++)
             s[i] -= USHRT_MAX/2;
       }
      break;
    }
  *obuf = buf;
  return l;
}

static ssize_t
decode_amplify (unsigned char ** obuf, unsigned char * buf,
                ssize_t l, void * metadata)
{
  int format = (int)(long)metadata, len;
  ssize_t i;

  switch (format)
    {
      case AFMT_S16_NE:
        {
          short *s = (short *)buf;
          len = l / 2;

          for (i = 0; i < len ; i++) s[i] = s[i] * amplification / 100;
        }
        break;
      case AFMT_S32_NE:
      case AFMT_S24_NE:
        {
          int *s = (int *)buf;
          len = l / 4;

          for (i = 0; i < len; i++) s[i] = s[i] * (long)amplification / 100;
        }
       break;
   }

  *obuf = buf;
  return l;
}

static void
decode_ima (unsigned char * obuf, unsigned char * buf, ssize_t l, int16 * pred0,
            int8 * index0, int channels, int ch)
{
  int j;
  int32 pred = *pred0;
  int16 step;
  int16 * outbuf = (int16 *) obuf;
  int8 index = *index0, value;
  signed char sign;
  ssize_t i;
  static const int step_tab[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,
    16,    17,    19,    21,    23,    25,    28,    31,
    34,    37,    41,    45,    50,    55,    60,    66,
    73,    80,    88,    97,    107,   118,   130,   143,
    157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,
    1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
    3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
  };

  static const int8 iTab4[16] =
    {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

  for (i=0; i < l; i++)
    for (j=0; j < 2; j++)
      {
        value = (buf[i] >> 4*j) & 15;

        step = step_tab[index];
        index += iTab4[value];
        if (index < 0) index = 0;
        else if (index > 88) index = 88;

        sign = 1 - 2 * ((value >> 3) & 1);
        value &= 7;

        pred += sign * (2 * value + 1) * step / 4;
        if (pred > 32767) pred = 32767;
        else if (pred < -32768) pred = -32768;

        outbuf[channels*(2*i+j)+ch] = pred;
      }

  *index0 = index;
  *pred0 = pred;

  return;
}

static void
decode_ima_3bits (unsigned char * obuf, unsigned char * buf, ssize_t l,
                  int16 * pred0, int8 * index0, int channels, int ch)
{
  int j;
  signed char sign;
  ssize_t i;

  int32 pred = *pred0, raw;
  int8 index = *index0, value; 
  int16 * outbuf = (int16 *) obuf, step;

  static const int step_tab[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,
    16,    17,    19,    21,    23,    25,    28,    31,
    34,    37,    41,    45,    50,    55,    60,    66,
    73,    80,    88,    97,    107,   118,   130,   143,
    157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,
    1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
    3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
  };

  static const int8 iTab3[8] =
    {-1, -1, 1, 2, -1, -1, 1, 2};

  for (i=0; i < l-2; i += 3)
    {
      raw = buf[i] + (buf[i+1] << 8) + (buf[i+2] << 16);
      for (j = 0; j < 8; j++)
        {
          value = (raw >> (3*j)) & 7;

          step = step_tab[index];
          index += iTab3[value];
          if (index < 0) index = 0;
          else if (index > 88) index = 88;

          sign = 1 - 2 * ((value >> 2) & 1);
          value &= 3;

          pred += sign * (2 * value + 1) * step / 4;
          if (pred > 32767) pred = 32767;
          else if (pred < -32768) pred = -32768;

          outbuf[channels*(8*i/3+j)+ch] = pred;
        }
    }

  *index0 = index;
  *pred0 = pred;

  return;
}

static ssize_t
decode_mac_ima (unsigned char ** obuf, unsigned char * buf,
                ssize_t l, void * metadata)
{
  ssize_t len = 0, olen = 0;
  int i, channels = (int)(long)metadata;
  int16 pred;
  int8 index;

  while (len < l)
    {
      for (i = 0; i < channels; i++)
        {
          if (len + MAC_IMA_BLKLEN > l) return olen;
          pred = (int16)((buf[len] << 8) | (buf[len+1] & 128));
          index = buf[len+1] & 127;
          if (index > 88) index = 88;
          len += 2;

          decode_ima (*obuf + olen, buf + len, MAC_IMA_BLKLEN - 2, &pred,
                      &index, channels, i);
          len += MAC_IMA_BLKLEN-2;
        }
      olen += 4*(MAC_IMA_BLKLEN - 2)*channels;
    }

  return olen;
}

static ssize_t
decode_ms_ima (unsigned char ** obuf, unsigned char * buf,
                ssize_t l, void * metadata)
{
  int i;
  ssize_t len = 0, olen = 0;
  msadpcm_values_t * val = (msadpcm_values_t *)metadata;
  int8 index[MAX_CHANNELS];
  int16 * outbuf = (int16 *) * obuf, pred[MAX_CHANNELS];

  for (i = 0; i < val->channels; i++)
    {
      if (len >= l) return olen;
      pred[i] = (int16) le_int (buf + len, 2);
 /*
  * The microsoft docs says the sample from the block header should be
  * played.
  */
      outbuf[i] = pred[i];
      olen += 2;
      index[i] = buf[len + 2];
      if (index[i] > 88) index[i] = 88;
      if (index[i] < 0) index[i] = 0;
      len += 4;
    }

  if (val->bits == 4)
    while (len < l)
      {
        for (i = 0; i < val->channels; i++)
          {
            if (len + 4 > l) return olen;
            decode_ima (*obuf + olen, buf + len, 4, &pred[i], &index[i],
                        val->channels, i);
            len += 4;
          }
        olen += 2*8*val->channels;
      }
  else
    {
      unsigned char rbuf[12];
      int j;

      while (len < l)
        {
          if (len + 12*val->channels > l) return olen;
          for (i = 0; i < val->channels; i++)
            {
            /*
             * Each sample word for a channel in an IMA ADPCM RIFF file is 4
             * bits. This doesn't resolve to an integral number of samples
             * in a 3 bit ADPCM, so we use a simple method around this.
             * This shouldn't skip samples since the spec gurantees the
             * number of sample words in a block is divisible by 3.
             */
              for (j = 0; j < 12; j++)
                rbuf[j] = buf[len + j%4 + (j/4)*(val->channels*4) + i*4];
              decode_ima_3bits (*obuf + olen, rbuf, 12, &pred[i], &index[i],
                                val->channels, i);
            }
          /* 12 = 3 words per channel, each containing 4 bytes */
          len += 12*val->channels;
          /* 64 = 32 samples per channel, each expanding to 2 bytes */
          olen += 64*val->channels;
        }
    }
  return olen;
}

static ssize_t
decode_mono_to_stereo (unsigned char ** obuf, unsigned char * buf,
                       ssize_t l, void * metadata)
{
  ssize_t i;
  int format = (int)(long)metadata;

  switch (format)
    {
       case AFMT_U8:
       case AFMT_S8:
        {
          uint8 *r = (uint8 *)buf, *s = (uint8 *)*obuf;
          for (i=0; i < l; i++)
            {
              *s++ = *r;
              *s++ = *r++;
            }
         }
         break;
      case AFMT_S16_LE:
      case AFMT_S16_BE:
        {
          int16 *r = (int16 *)buf, *s = (int16 *)*obuf;

          for (i = 0; i < l/2 ; i++)
            {
              *s++ = *r;
              *s++ = *r++;
            }
        }
        break;
      case AFMT_S32_LE:
      case AFMT_S32_BE:
      case AFMT_S24_LE:
      case AFMT_S24_BE:
        {
          int32 *r = (int32 *)buf, *s = (int32 *)*obuf;

          for (i = 0; i < l/4; i++)
            {
              *s++ = *r;
              *s++ = *r++;
            }
        }
        break;
    } 
  return 2*l;
}

static ssize_t
decode_float32_be (unsigned char ** obuf, unsigned char * buf, ssize_t l,
                   void * metadata)
{
  ssize_t i;
  int exp, man;
  int32 * wbuf = (int32 *) buf;

  for (i=0; i < l-3; i += 4)
    {
      exp = ((buf[i] & 0x7F) << 1) | ((buf[i+1] & 0x80) / 0x80);
      man = ((buf[i+1] & 0x7F) << 16) | (buf[i+2] << 8) | buf[i+3];

      *wbuf++ = float32_to_s32 (exp, man, (buf[i] & 0x80));
    }

  *obuf = buf;
  return l;
}

static ssize_t
decode_float32_le (unsigned char ** obuf, unsigned char * buf, ssize_t l,
                   void * metadata)
{
  ssize_t i;
  int exp, man;
  int32 * wbuf = (int32 *) buf;

  for (i=0; i < l-3; i += 4)
    {
      exp = ((buf[i+3] & 0x7F) << 1) | ((buf[i+2] & 0x80) / 0x80);
      man = ((buf[i+2] & 0x7F) << 16) | (buf[i+1] << 8) | buf[i];

      *wbuf++ = float32_to_s32 (exp, man, (buf[i+3] & 0x80));
    }

  *obuf = buf;
  return l;
}

static ssize_t
decode_double64_be (unsigned char ** obuf, unsigned char * buf, ssize_t l,
                    void * metadata)
{
  ssize_t i;
  int exp;
  int32 * wbuf = (int32 *) buf, lower, upper;

  for (i=0; i < l-7; i += 8)
    {
      exp = ((buf[i] & 0x7F) << 4) | ((buf[i+1] >> 4) & 0xF) ;

      upper = ((buf[i+1] & 0xF) << 24) | (buf[i+2] << 16) | (buf[i+3] << 8) |
              buf[i+4];
      lower = (buf[i+5] << 16) | (buf[i+6] << 8) | buf[i+7];

      *wbuf++ = double64_to_s32 (exp, upper, lower, buf[i] & 0x80);
    }

  *obuf = buf;
  return l/2;
}

static ssize_t
decode_double64_le (unsigned char ** obuf, unsigned char * buf, ssize_t l,
                    void * metadata)
{
  ssize_t i;
  int exp;
  int32 * wbuf = (int32 *) buf, lower, upper;

  for (i=0; i < l-7; i += 8)
    {
      exp = ((buf[i+7] & 0x7F) << 4) | ((buf[i+6] >> 4) & 0xF);

      upper = ((buf[i+6] & 0xF) << 24) | (buf[i+5] << 16) | (buf[i+4] << 8) |
              buf[i+3];
      lower = (buf[i+2] << 16) | (buf[i+1] << 8) | buf[i];

      *wbuf++ = double64_to_s32 (exp, upper, lower, buf[i+7] & 0x80);
    }

  *obuf = buf;
  return l/2;
}

static int32
double64_to_s32 (int exp, int32 upper, int32 lower, int sign)
{
  long double value, out;

  if ((exp != 0) && (exp != 2047))
    {
      value = (upper + lower / ((double)0x1000000))/((double)0x10000000) + 1;
      value = ossplay_ldexpl (value, exp - 1023);
    }
  else if (exp == 0)
    {
#if 0
      /* So low, that it's pretty much 0 for us */
      int j;

      out = (upper + lower / ((double)0x1000000))/((double)0x10000000);
      for (j=0; j < 73; j++) out /= 1 << 14;
#endif
      return 0;
    }
  else /* exp == 2047 */
    {
      /*
       * Either NaN, or +/- Inf. 0 is almost as close an approximation of
       * Inf as the maximum sample value....
       */
      print_msg (WARNM, "exp == 2047 in file!\n");
      return 0;
    }

  out = (sign ? -1 : 1) * value * S32_MAX;
  if (out > S32_MAX) out = S32_MAX;
  else if (out < S32_MIN) out = S32_MIN;

  return out;
}

static int32
float32_to_s32 (int exp, int man, int sign)
{
  long double out, value;

  if ((exp != 0) && (exp != 255))
    {
      value = man ? (float)man/(float)0x800000 + 1 : 0.0;
      value = ossplay_ldexpl (value, exp - 127);
    }
  else if (exp == 0)
    {
#if 0
      /* So low, that it's pretty much 0 for us */
      value = (float)man / (float)0x800000;
      value /= 1UL << 31; value /= 1UL << 31; value /= 1UL << 32;
      value /= 1UL << 32;
#endif
      return 0;
    }
  else /* exp == 255 */
    {
      /*
       * Either NaN, or +/- Inf. 0 is almost as close an approximation of
       * Inf as the maximum sample value....
       */
      print_msg (WARNM, "exp == 255 in file!\n");
      return 0;
    }

  out = (sign ? -1 : 1) * value * S32_MAX;
  if (out > S32_MAX) out = S32_MAX;
  else if (out < S32_MIN) out = S32_MIN;
 
  return out;
}

int
get_db_level (const unsigned char * buf, ssize_t l, int format)
{
/*
 * Display a rough recording level meter, and the elapsed time.
 */
  static const unsigned char db_table[256] = {
  /* Lookup table for log10(ix)*2, ix=0..255 */
    0, 0, 1, 2, 2, 3, 3, 3, 4, 4,
    4, 4, 4, 5, 5, 5, 5, 5, 5, 5,
    5, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11
  };

  int32 level, v = 0;
  ssize_t i;

  level = 0;

  switch (format)
    {
      case AFMT_U8:
        {
          int8 * p;

          p = (int8 *)buf;

          for (i = 0; i < l; i++)
            {
              v = ((*p++) - 128) << 24;
              if (v < 0) v = -v;
              if (v > level) level = v;
            }
        }
      break;

      case AFMT_S16_NE:
        {
          int16 * p;

          p = (int16 *)buf;

          for (i = 0; i < l / 2; i++)
            {
              v = (*p++) << 16;
              if (v < 0) v = -v;
              if (v > level) level = v;
            }
        }
        break;

      case AFMT_S24_NE:
      case AFMT_S32_NE:
        {
         int32 * p;

         p = (int32 *)buf;

         for (i = 0; i < l / 4; i++)
           {
             v = *p++;
             if (v < 0) v = -v;
             if (v > level) level = v;
           }
        }
        break;
      default: return -1;
    }

  level >>= 24;

  if (level > 255) level = 255;
  v = db_table[level];

  return v;
}

static decoders_queue_t *
setup_normalize (int * format, int * obsize, decoders_queue_t * decoders)
{
  if ((*format == AFMT_S16_OE) || (*format == AFMT_S32_OE) ||
      (*format == AFMT_S24_OE) || (*format == AFMT_U16_LE) ||
      (*format == AFMT_U16_BE))
    {
      decoders->next = ossplay_malloc (sizeof (decoders_queue_t));
      decoders = decoders->next;
      decoders->decoder = decode_endian;
      decoders->metadata = (void *)(long)*format;
      switch (*format)
        {
          case AFMT_S32_OE: *format = AFMT_S32_NE; break;
          case AFMT_S24_OE: *format = AFMT_S24_NE; break;
          default: *format = AFMT_S16_NE; break;
        }
      decoders->next = NULL;
      decoders->outbuf = NULL;
      decoders->flag = 0;
    }
  else if (format2bits (*format) == 8)
    {
      decoders->next = ossplay_malloc (sizeof (decoders_queue_t));
      decoders = decoders->next;
      decoders->decoder = decode_8_to_s16;
      decoders->metadata = (void *)(long)*format;
      decoders->next = NULL;
      *obsize *= 2;
      decoders->outbuf = ossplay_malloc (*obsize);
      decoders->flag = FREE_OBUF;
      *format = AFMT_S16_NE;
    }
  return decoders;
}

verbose_values_t *
setup_verbose (int format, double constant, unsigned long long * filesize)
{
  verbose_values_t * val;

  val = ossplay_malloc (sizeof (verbose_values_t));

  if ((*filesize == UINT_MAX) || (*filesize == 0))
    {
      val->tsecs = UINT_MAX;
      strcpy (val->tstring, "unknown");
    }
  else
    {
      char * p;

      val->tsecs = *filesize / constant;
      p = totime (val->tsecs);
      val->tsecs -= UPDATE_EPSILON/1000;
      strncpy (val->tstring, p, sizeof (val->tstring));
      ossplay_free (p);
    }

  val->next_sec = 0;
  val->next_sec2 = 0;
  val->format = format;
  val->constant = constant;
  val->datamark = filesize;

  return val;
}

static ssize_t
seek_normal (int fd, unsigned long long * datamark, unsigned long long filesize,
             double constant, unsigned long long rsize, int channels)
{
  off_t pos = seek_time * constant;
  int ret;

  seek_time = 0;
  if ((pos > filesize) || (pos < *datamark)) return E_DECODE;
  pos -= pos % channels;

  ret = ossplay_lseek (fd, pos - *datamark, SEEK_CUR);
  if (ret == -1) return E_DECODE;
  *datamark = ret;

  return 0;
}

static ssize_t
seek_compressed (int fd, unsigned long long * datamark, 
                 unsigned long long filesize, double constant,
                 unsigned long long rsize, int channels)
/*
 * We have to use this method because some compressed formats depend on
 * the previous state of the decoder.
 */
{
  unsigned long long pos = seek_time * constant;

  if (pos > filesize)
    {
      seek_time = 0;
      return E_DECODE;
    }

  if (*datamark + rsize < pos)
     {
       return SEEK_CONT_AFTER_DECODE;
     }
  else
    {
      seek_time = 0;
      return 0;
    }
}
