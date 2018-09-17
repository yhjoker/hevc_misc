/* The copyright in this software is being made available under the BSD
 * License, included below. This software may be subject to other third party
 * and contributor rights, including patent rights, and no such rights are
 * granted under this license.
 *
 * Copyright (c) 2010-2017, ITU/ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the ITU/ISO/IEC nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/** \file     TComPicYuv.cpp
    \brief    picture YUV buffer class
*/

#include <cstdlib>
#include <assert.h>
#include <memory.h>

#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

#include "TComPicYuv.h"
#include "TLibVideoIO/TVideoIOYuv.h"

//! \ingroup TLibCommon
//! \{

TComPicYuv::TComPicYuv()
{
  for(UInt i=0; i<MAX_NUM_COMPONENT; i++)
  {
    m_apiPicBuf[i]    = NULL;   // Buffer (including margin)
    m_piPicOrg[i]     = NULL;    // m_apiPicBufY + m_iMarginLuma*getStride() + m_iMarginLuma
  }

  for(UInt i=0; i<MAX_NUM_CHANNEL_TYPE; i++)
  {
    m_ctuOffsetInBuffer[i]=0;
    m_subCuOffsetInBuffer[i]=0;
  }

  m_bIsBorderExtended = false;
}




TComPicYuv::~TComPicYuv()
{
  destroy();
}



Void TComPicYuv::createWithoutCUInfo ( const Int picWidth,                 ///< picture width
                                       const Int picHeight,                ///< picture height
                                       const ChromaFormat chromaFormatIDC, ///< chroma format
                                       const Bool bUseMargin,              ///< if true, then a margin of uiMaxCUWidth+16 and uiMaxCUHeight+16 is created around the image.
                                       const UInt maxCUWidth,              ///< used for margin only
                                       const UInt maxCUHeight)             ///< used for margin only

{
  destroy();

  m_picWidth          = picWidth;
  m_picHeight         = picHeight;
  m_chromaFormatIDC   = chromaFormatIDC;
  m_marginX          = (bUseMargin ? maxCUWidth : 0 ) + 16;   // for 16-byte alignment
  m_marginY          = (bUseMargin ? maxCUHeight : 0 ) + 16;  // margin for 8-tap filter and infinite padding
  m_bIsBorderExtended = false;

  // assign the picture arrays and set up the ptr to the top left of the original picture
  for(UInt comp=0; comp<getNumberValidComponents(); comp++)   //NumberValidComponents：1 for 400 或者最大
  {
    const ComponentID ch=ComponentID(comp);
    m_apiPicBuf[comp] = (Pel*)xMalloc( Pel, getStride(ch) * getTotalHeight(ch));  //margin后的宽度*margin后的高度
    m_piPicOrg[comp]  = m_apiPicBuf[comp] + (m_marginY >> getComponentScaleY(ch)) * getStride(ch) + (m_marginX >> getComponentScaleX(ch));
  }     //这里实际分配了缓冲区的空间，piPicorg处存放的是实际的图像帧开始处
  // initialize pointers for unused components to NULL
  for(UInt comp=getNumberValidComponents();comp<MAX_NUM_COMPONENT; comp++)
  {
    m_apiPicBuf[comp] = NULL;
    m_piPicOrg[comp]  = NULL;
  }//其他分量设置为NULL

  for(Int chan=0; chan<MAX_NUM_CHANNEL_TYPE; chan++)
  {
    m_ctuOffsetInBuffer[chan]   = NULL;
    m_subCuOffsetInBuffer[chan] = NULL;
  }
}



Void TComPicYuv::create ( const Int picWidth,                 ///< picture width
                          const Int picHeight,                ///< picture height
                          const ChromaFormat chromaFormatIDC, ///< chroma format
                          const UInt maxCUWidth,              ///< used for generating offsets to CUs.
                          const UInt maxCUHeight,             ///< used for generating offsets to CUs.
                          const UInt maxCUDepth,              ///< used for generating offsets to CUs.
                          const Bool bUseMargin)              ///< if true, then a margin of uiMaxCUWidth+16 and uiMaxCUHeight+16 is created around the image.

{
  //这个函数的参数包括图像的宽*高，是否使用对齐等（都使用了对齐），主要是分配了空间
  createWithoutCUInfo(picWidth, picHeight, chromaFormatIDC, bUseMargin, maxCUWidth, maxCUHeight);


  const Int numCuInWidth  = m_picWidth  / maxCUWidth  + (m_picWidth  % maxCUWidth  != 0);
  const Int numCuInHeight = m_picHeight / maxCUHeight + (m_picHeight % maxCUHeight != 0);   //设置水平和垂直方向上CTU的数量
  for(Int chan=0; chan<MAX_NUM_CHANNEL_TYPE; chan++)
  {
    const ChannelType ch= ChannelType(chan);    //指代是luma或chroma
    const Int ctuHeight = maxCUHeight>>getChannelTypeScaleY(ch);    //判断的是不同采样分量是否需要进行减半
    const Int ctuWidth  = maxCUWidth>>getChannelTypeScaleX(ch);
    const Int stride    = getStride(ch);

    m_ctuOffsetInBuffer[chan] = new Int[numCuInWidth * numCuInHeight];    //每个分量均需要记录CTU的空间

    for (Int cuRow = 0; cuRow < numCuInHeight; cuRow++)
    {
      for (Int cuCol = 0; cuCol < numCuInWidth; cuCol++)
      {
        m_ctuOffsetInBuffer[chan][cuRow * numCuInWidth + cuCol] = stride * cuRow * ctuHeight + cuCol * ctuWidth;
      }   //这里记录了CTU的偏移
    }

    m_subCuOffsetInBuffer[chan] = new Int[(size_t)1 << (2 * maxCUDepth)]; //用于记录子CU的偏移,与可划分的Cu数量保持一致，按最小Cu划分

    const Int numSubBlockPartitions=(1<<maxCUDepth);
    const Int minSubBlockHeight    =(ctuHeight >> maxCUDepth);
    const Int minSubBlockWidth     =(ctuWidth  >> maxCUDepth);

    for (Int buRow = 0; buRow < numSubBlockPartitions; buRow++)
    {
      for (Int buCol = 0; buCol < numSubBlockPartitions; buCol++)
      {
        m_subCuOffsetInBuffer[chan][(buRow << maxCUDepth) + buCol] = stride  * buRow * minSubBlockHeight + buCol * minSubBlockWidth;
      }   //存放每个最小块CU的offset
    }
  }
}

Void TComPicYuv::destroy()
{
  for(Int comp=0; comp<MAX_NUM_COMPONENT; comp++)
  {
    m_piPicOrg[comp] = NULL;

    if( m_apiPicBuf[comp] )
    {
      xFree( m_apiPicBuf[comp] );
      m_apiPicBuf[comp] = NULL;
    }
  }

  for(UInt chan=0; chan<MAX_NUM_CHANNEL_TYPE; chan++)
  {
    if (m_ctuOffsetInBuffer[chan])
    {
      delete[] m_ctuOffsetInBuffer[chan];
      m_ctuOffsetInBuffer[chan] = NULL;
    }
    if (m_subCuOffsetInBuffer[chan])
    {
      delete[] m_subCuOffsetInBuffer[chan];
      m_subCuOffsetInBuffer[chan] = NULL;
    }
  }
}



Void  TComPicYuv::copyToPic (TComPicYuv*  pcPicYuvDst) const
{
  assert( m_chromaFormatIDC == pcPicYuvDst->getChromaFormat() );

  for(Int comp=0; comp<getNumberValidComponents(); comp++)
  {
    const ComponentID compId=ComponentID(comp);
    const Int width     = getWidth(compId);
    const Int height    = getHeight(compId);
    const Int strideSrc = getStride(compId);
    assert(pcPicYuvDst->getWidth(compId) == width);
    assert(pcPicYuvDst->getHeight(compId) == height);
    if (strideSrc==pcPicYuvDst->getStride(compId))
    {
      ::memcpy ( pcPicYuvDst->getBuf(compId), getBuf(compId), sizeof(Pel)*strideSrc*getTotalHeight(compId));
    }
    else
    {
      const Pel *pSrc       = getAddr(compId);
            Pel *pDest      = pcPicYuvDst->getAddr(compId);
      const UInt strideDest = pcPicYuvDst->getStride(compId);

      for(Int y=0; y<height; y++, pSrc+=strideSrc, pDest+=strideDest)
      {
        ::memcpy(pDest, pSrc, width*sizeof(Pel));
      }
    }
  }
}


Void TComPicYuv::extendPicBorder ()
{
  if ( m_bIsBorderExtended )
  {
    return;
  }

  for(Int comp=0; comp<getNumberValidComponents(); comp++)
  {
    const ComponentID compId=ComponentID(comp);
    Pel *piTxt=getAddr(compId); // piTxt = point to (0,0) of image within bigger picture.
    const Int stride=getStride(compId);
    const Int width=getWidth(compId);
    const Int height=getHeight(compId);
    const Int marginX=getMarginX(compId);
    const Int marginY=getMarginY(compId);

    Pel*  pi = piTxt;
    // do left and right margins
    for (Int y = 0; y < height; y++)
    {
      for (Int x = 0; x < marginX; x++ )
      {
        pi[ -marginX + x ] = pi[0];
        pi[    width + x ] = pi[width-1];
      }
      pi += stride;
    }

    // pi is now the (0,height) (bottom left of image within bigger picture
    pi -= (stride + marginX);
    // pi is now the (-marginX, height-1)
    for (Int y = 0; y < marginY; y++ )
    {
      ::memcpy( pi + (y+1)*stride, pi, sizeof(Pel)*(width + (marginX<<1)) );
    }

    // pi is still (-marginX, height-1)
    pi -= ((height-1) * stride);
    // pi is now (-marginX, 0)
    for (Int y = 0; y < marginY; y++ )
    {
      ::memcpy( pi - (y+1)*stride, pi, sizeof(Pel)*(width + (marginX<<1)) );
    }
  }

  m_bIsBorderExtended = true;
}



// NOTE: This function is never called, but may be useful for developers.可用于打印某些调试信息
Void TComPicYuv::dump (const std::string &fileName, const BitDepths &bitDepths, const Bool bAppend, const Bool bForceTo8Bit) const
{
  FILE *pFile = fopen (fileName.c_str(), bAppend?"ab":"wb");

  Bool is16bit=false;
  for(Int comp = 0; comp < getNumberValidComponents() && !bForceTo8Bit; comp++)
  {
    if (bitDepths.recon[toChannelType(ComponentID(comp))]>8)
    {
      is16bit=true;
    }
  }

  for(Int comp = 0; comp < getNumberValidComponents(); comp++)
  {
    const ComponentID  compId = ComponentID(comp);
    const Pel         *pi     = getAddr(compId);
    const Int          stride = getStride(compId);
    const Int          height = getHeight(compId);
    const Int          width  = getWidth(compId);

    if (is16bit)
    {
      for (Int y = 0; y < height; y++ )
      {
        for (Int x = 0; x < width; x++ )
        {
          UChar uc = (UChar)((pi[x]>>0) & 0xff);
          fwrite( &uc, sizeof(UChar), 1, pFile );
          uc = (UChar)((pi[x]>>8) & 0xff);
          fwrite( &uc, sizeof(UChar), 1, pFile );
        }
        pi += stride;
      }
    }
    else
    {
      const Int shift  = bitDepths.recon[toChannelType(compId)] - 8;
      const Int offset = (shift>0)?(1<<(shift-1)):0;
      for (Int y = 0; y < height; y++ )
      {
        for (Int x = 0; x < width; x++ )
        {
          UChar uc = (UChar)Clip3<Pel>(0, 255, (pi[x]+offset)>>shift);
          fwrite( &uc, sizeof(UChar), 1, pFile );
        }
        pi += stride;
      }
    }
  }

  fclose(pFile);
}

//! \}
