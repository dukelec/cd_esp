/*
 * Software License Agreement (MIT License)
 *
 * Copyright (c) 2017, DUKELEC, Inc.
 * All rights reserved.
 *
 * Author: Duke Fong <d@d-l.io>
 */

#ifndef __CD_CONFIG_H__
#define __CD_CONFIG_H__

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32C3
#define CDCTL_OSC_CLK       40000000UL // xtal 40MHz, output on mco pin
#else // c5
#define CDCTL_OSC_CLK       48000000UL // xtal 48MHz, output on mco pin
#endif

#define CD_ARCH_SPI_DMA

#define CD_FRAME_SIZE       258
#define CD_FRAME_TYPE       "cd_frame.h" // custom cd_frame_t
#define CDN_MAX_PAYLOAD     251

#define CD_DEBUG
//#define CD_VERBOSE
//#define CD_LIST_DEBUG

//#define CD_SMP
#define CD_LIST_IT
#define CD_IRQ_SAFE

#define CDUART_IDLE_TIME    50 // ms


#include "main.h"
#include "debug_config.h"

#endif
