/*
 * sound/lynx_hal.c
 *
 * Driver for LynxONE Studio Interface.
 */
#define COPYING Copyright (C) Hannu Savolainen and Dev Mazumdar 2001. All rights reserved.

#include "lynxone_cfg.h"
#define OSS
#include "Environ.h"
#include "HalLynx.h"
#include "HalAudio.h"
#include "HalDwnld.h"
#include "DosCmn.h"
#include "DrvDebug.h"
#include "Pathfind.h"
#include "lynx_hal.h"

#include "HalDwnld.inc"
#include "HalLynx.inc"
