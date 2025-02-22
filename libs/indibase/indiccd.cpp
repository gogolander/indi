/*******************************************************************************
 Copyright(c) 2010-2018 Jasem Mutlaq. All rights reserved.

 Copyright(c) 2010, 2011 Gerry Rozema. All rights reserved.

 Rapid Guide support added by CloudMakers, s. r. o.
 Copyright(c) 2013 CloudMakers, s. r. o. All rights reserved.

 Star detection algorithm is based on PHD Guiding by Craig Stark
 Copyright (c) 2006-2010 Craig Stark. All rights reserved.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License version 2 as published by the Free Software Foundation.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.

 You should have received a copy of the GNU Library General Public License
 along with this library; see the file COPYING.LIB.  If not, write to
 the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 Boston, MA 02110-1301, USA.
*******************************************************************************/

// use 64-bit values when calling stat()
#define _FILE_OFFSET_BITS 64

#include "indiccd.h"

#include "fpack/fpack.h"
#include "indicom.h"
#include "locale_compat.h"
#include "indiutility.h"

#include <fitsio.h>

#include <libnova/julian_day.h>
#include <libnova/precession.h>
#include <libnova/airmass.h>
#include <libnova/transform.h>
#include <libnova/ln_types.h>
#include <libastro.h>

#include <cmath>
#include <regex>

#include <dirent.h>
#include <cerrno>
#include <cstdlib>
#include <zlib.h>
#include <sys/stat.h>

const char * IMAGE_SETTINGS_TAB = "Image Settings";
const char * IMAGE_INFO_TAB     = "Image Info";
const char * GUIDE_HEAD_TAB     = "Guider Head";
//const char * RAPIDGUIDE_TAB     = "Rapid Guide";

#ifdef HAVE_WEBSOCKET
uint16_t INDIWSServer::m_global_port = 11623;
#endif

namespace INDI
{

CCD::CCD()
{
    //ctor
    capability = 0;

    InExposure              = false;
    InGuideExposure         = false;
    //RapidGuideEnabled       = false;
    //GuiderRapidGuideEnabled = false;
    m_ValidCCDRotation        = false;

    AutoLoop         = false;
    SendImage        = false;
    ShowMarker       = false;
    GuiderAutoLoop   = false;
    GuiderSendImage  = false;
    GuiderShowMarker = false;

    ExposureTime       = 0.0;
    GuiderExposureTime = 0.0;
    CurrentFilterSlot  = -1;

    RA              = std::numeric_limits<double>::quiet_NaN();
    Dec             = std::numeric_limits<double>::quiet_NaN();
    pierSide        = -1;
    J2000RA         = std::numeric_limits<double>::quiet_NaN();
    J2000DE         = std::numeric_limits<double>::quiet_NaN();
    J2000Valid      = false;
    MPSAS           = std::numeric_limits<double>::quiet_NaN();
    RotatorAngle    = std::numeric_limits<double>::quiet_NaN();
    // JJ ed 2019-12-10
    FocuserPos      = -1;
    FocuserTemp     = std::numeric_limits<double>::quiet_NaN();

    Airmass         = std::numeric_limits<double>::quiet_NaN();
    Latitude        = std::numeric_limits<double>::quiet_NaN();
    Longitude       = std::numeric_limits<double>::quiet_NaN();
    Azimuth         = std::numeric_limits<double>::quiet_NaN();
    Altitude        = std::numeric_limits<double>::quiet_NaN();
    primaryAperture = std::numeric_limits<double>::quiet_NaN();
    primaryFocalLength = std::numeric_limits<double>::quiet_NaN();
    guiderAperture = std::numeric_limits<double>::quiet_NaN();
    guiderFocalLength = std::numeric_limits<double>::quiet_NaN();

    // Check temperature every 5 seconds.
    m_TemperatureCheckTimer.setInterval(5000);
    m_TemperatureCheckTimer.callOnTimeout(std::bind(&CCD::checkTemperatureTarget, this));

    exposureStartTime[0] = 0;
    exposureDuration = 0.0;
}

CCD::~CCD()
{
    // Only update if index is different.
    if (m_ConfigFastExposureIndex != IUFindOnSwitchIndex(&FastExposureToggleSP))
        saveConfig(true, FastExposureToggleSP.name);
}

void CCD::SetCCDCapability(uint32_t cap)
{
    capability = cap;

    if (HasST4Port())
        setDriverInterface(getDriverInterface() | GUIDER_INTERFACE);
    else
        setDriverInterface(getDriverInterface() & ~GUIDER_INTERFACE);

    syncDriverInfo();
    HasStreaming();
    HasDSP();
}

bool CCD::initProperties()
{
    DefaultDevice::initProperties(); //  let the base class flesh in what it wants

    // CCD Temperature
    IUFillNumber(&TemperatureN[0], "CCD_TEMPERATURE_VALUE", "Temperature (C)", "%5.2f", -50.0, 50.0, 0., 0.);
    IUFillNumberVector(&TemperatureNP, TemperatureN, 1, getDeviceName(), "CCD_TEMPERATURE", "Temperature",
                       MAIN_CONTROL_TAB, IP_RW, 60, IPS_IDLE);

    // Camera temperature ramp
    TemperatureRampNP[RAMP_SLOPE].fill("RAMP_SLOPE", "Max. dT (C/min)", "%.f", 0, 30, 1, 0);
    TemperatureRampNP[RAMP_THRESHOLD].fill("RAMP_THRESHOLD", "Threshold (C)", "%.1f", 0.1, 2, 0.1, 0.2);
    TemperatureRampNP.fill(getDeviceName(), "CCD_TEMP_RAMP", "Temp. Ramp", MAIN_CONTROL_TAB, IP_RW, 60, IPS_IDLE);

    /**********************************************/
    /**************** Primary Chip ****************/
    /**********************************************/

    // Primary CCD Region-Of-Interest (ROI)
    IUFillNumber(&PrimaryCCD.ImageFrameN[CCDChip::FRAME_X], "X", "Left ", "%4.0f", 0, 0.0, 0, 0);
    IUFillNumber(&PrimaryCCD.ImageFrameN[CCDChip::FRAME_Y], "Y", "Top", "%4.0f", 0, 0, 0, 0);
    IUFillNumber(&PrimaryCCD.ImageFrameN[CCDChip::FRAME_W], "WIDTH", "Width", "%4.0f", 0, 0.0, 0, 0.0);
    IUFillNumber(&PrimaryCCD.ImageFrameN[CCDChip::FRAME_H], "HEIGHT", "Height", "%4.0f", 0, 0, 0, 0.0);
    IUFillNumberVector(&PrimaryCCD.ImageFrameNP, PrimaryCCD.ImageFrameN, 4, getDeviceName(), "CCD_FRAME", "Frame",
                       IMAGE_SETTINGS_TAB, IP_RW, 60, IPS_IDLE);

    // Primary CCD Frame Type
    IUFillSwitch(&PrimaryCCD.FrameTypeS[CCDChip::LIGHT_FRAME], "FRAME_LIGHT", "Light", ISS_ON);
    IUFillSwitch(&PrimaryCCD.FrameTypeS[CCDChip::BIAS_FRAME], "FRAME_BIAS", "Bias", ISS_OFF);
    IUFillSwitch(&PrimaryCCD.FrameTypeS[CCDChip::DARK_FRAME], "FRAME_DARK", "Dark", ISS_OFF);
    IUFillSwitch(&PrimaryCCD.FrameTypeS[CCDChip::FLAT_FRAME], "FRAME_FLAT", "Flat", ISS_OFF);
    IUFillSwitchVector(&PrimaryCCD.FrameTypeSP, PrimaryCCD.FrameTypeS, 4, getDeviceName(), "CCD_FRAME_TYPE",
                       "Type", IMAGE_SETTINGS_TAB, IP_RW, ISR_1OFMANY, 60, IPS_IDLE);

    // Primary CCD Exposure
    IUFillNumber(&PrimaryCCD.ImageExposureN[0], "CCD_EXPOSURE_VALUE", "Duration (s)", "%5.2f", 0.01, 3600, 1.0, 1.0);
    IUFillNumberVector(&PrimaryCCD.ImageExposureNP, PrimaryCCD.ImageExposureN, 1, getDeviceName(), "CCD_EXPOSURE",
                       "Expose", MAIN_CONTROL_TAB, IP_RW, 60, IPS_IDLE);

    // Primary CCD Abort
    IUFillSwitch(&PrimaryCCD.AbortExposureS[0], "ABORT", "Abort", ISS_OFF);
    IUFillSwitchVector(&PrimaryCCD.AbortExposureSP, PrimaryCCD.AbortExposureS, 1, getDeviceName(), "CCD_ABORT_EXPOSURE",
                       "Abort", MAIN_CONTROL_TAB, IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    // Primary CCD Binning
    IUFillNumber(&PrimaryCCD.ImageBinN[0], "HOR_BIN", "X", "%2.0f", 1, 4, 1, 1);
    IUFillNumber(&PrimaryCCD.ImageBinN[1], "VER_BIN", "Y", "%2.0f", 1, 4, 1, 1);
    IUFillNumberVector(&PrimaryCCD.ImageBinNP, PrimaryCCD.ImageBinN, 2, getDeviceName(), "CCD_BINNING", "Binning",
                       IMAGE_SETTINGS_TAB, IP_RW, 60, IPS_IDLE);

    // Primary CCD Info
    IUFillNumber(&PrimaryCCD.ImagePixelSizeN[CCDChip::CCD_MAX_X], "CCD_MAX_X", "Max. Width", "%.f", 1, 16000, 0, 0);
    IUFillNumber(&PrimaryCCD.ImagePixelSizeN[CCDChip::CCD_MAX_Y], "CCD_MAX_Y", "Max. Height", "%.f", 1, 16000, 0, 0);
    IUFillNumber(&PrimaryCCD.ImagePixelSizeN[CCDChip::CCD_PIXEL_SIZE], "CCD_PIXEL_SIZE", "Pixel size (um)", "%.2f", 1,
                 40, 0, 0);
    IUFillNumber(&PrimaryCCD.ImagePixelSizeN[CCDChip::CCD_PIXEL_SIZE_X], "CCD_PIXEL_SIZE_X", "Pixel size X", "%.2f", 1,
                 40, 0, 0);
    IUFillNumber(&PrimaryCCD.ImagePixelSizeN[CCDChip::CCD_PIXEL_SIZE_Y], "CCD_PIXEL_SIZE_Y", "Pixel size Y", "%.2f", 1,
                 40, 0, 0);
    IUFillNumber(&PrimaryCCD.ImagePixelSizeN[CCDChip::CCD_BITSPERPIXEL], "CCD_BITSPERPIXEL", "Bits per pixel", "%.f",
                 8, 64, 0, 0);
    IUFillNumberVector(&PrimaryCCD.ImagePixelSizeNP, PrimaryCCD.ImagePixelSizeN, 6, getDeviceName(), "CCD_INFO",
                       "CCD Information", IMAGE_INFO_TAB, IP_RO, 60, IPS_IDLE);

    // Primary CCD Compression Options
    IUFillSwitch(&PrimaryCCD.CompressS[INDI_ENABLED], "INDI_ENABLED", "Enabled", ISS_OFF);
    IUFillSwitch(&PrimaryCCD.CompressS[INDI_DISABLED], "INDI_DISABLED", "Disabled", ISS_ON);
    IUFillSwitchVector(&PrimaryCCD.CompressSP, PrimaryCCD.CompressS, 2, getDeviceName(), "CCD_COMPRESSION", "Compression",
                       IMAGE_SETTINGS_TAB, IP_RW, ISR_1OFMANY, 60, IPS_IDLE);
    PrimaryCCD.SendCompressed = false;

    // Primary CCD Chip Data Blob
    IUFillBLOB(&PrimaryCCD.FitsB, "CCD1", "Image", "");
    IUFillBLOBVector(&PrimaryCCD.FitsBP, &PrimaryCCD.FitsB, 1, getDeviceName(), "CCD1", "Image Data", IMAGE_INFO_TAB,
                     IP_RO, 60, IPS_IDLE);

    // Bayer
    IUFillText(&BayerT[0], "CFA_OFFSET_X", "X Offset", "0");
    IUFillText(&BayerT[1], "CFA_OFFSET_Y", "Y Offset", "0");
    IUFillText(&BayerT[2], "CFA_TYPE", "Filter", nullptr);
    IUFillTextVector(&BayerTP, BayerT, 3, getDeviceName(), "CCD_CFA", "Bayer Info", IMAGE_INFO_TAB, IP_RW, 60,
                     IPS_IDLE);

    // Reset Frame Settings
    IUFillSwitch(&PrimaryCCD.ResetS[0], "RESET", "Reset", ISS_OFF);
    IUFillSwitchVector(&PrimaryCCD.ResetSP, PrimaryCCD.ResetS, 1, getDeviceName(), "CCD_FRAME_RESET", "Frame Values",
                       IMAGE_SETTINGS_TAB, IP_WO, ISR_1OFMANY, 0, IPS_IDLE);

    /**********************************************/
    /********* Primary Chip Rapid Guide  **********/
    /**********************************************/
#if 0
    IUFillSwitch(&PrimaryCCD.RapidGuideS[0], "ENABLE", "Enable", ISS_OFF);
    IUFillSwitch(&PrimaryCCD.RapidGuideS[1], "DISABLE", "Disable", ISS_ON);
    IUFillSwitchVector(&PrimaryCCD.RapidGuideSP, PrimaryCCD.RapidGuideS, 2, getDeviceName(), "CCD_RAPID_GUIDE",
                       "Rapid Guide", OPTIONS_TAB, IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

    IUFillSwitch(&PrimaryCCD.RapidGuideSetupS[0], "AUTO_LOOP", "Auto loop", ISS_ON);
    IUFillSwitch(&PrimaryCCD.RapidGuideSetupS[1], "SEND_IMAGE", "Send image", ISS_OFF);
    IUFillSwitch(&PrimaryCCD.RapidGuideSetupS[2], "SHOW_MARKER", "Show marker", ISS_OFF);
    IUFillSwitchVector(&PrimaryCCD.RapidGuideSetupSP, PrimaryCCD.RapidGuideSetupS, 3, getDeviceName(),
                       "CCD_RAPID_GUIDE_SETUP", "Rapid Guide Setup", RAPIDGUIDE_TAB, IP_RW, ISR_NOFMANY, 0, IPS_IDLE);

    IUFillNumber(&PrimaryCCD.RapidGuideDataN[0], "GUIDESTAR_X", "Guide star position X", "%5.2f", 0, 1024, 0, 0);
    IUFillNumber(&PrimaryCCD.RapidGuideDataN[1], "GUIDESTAR_Y", "Guide star position Y", "%5.2f", 0, 1024, 0, 0);
    IUFillNumber(&PrimaryCCD.RapidGuideDataN[2], "GUIDESTAR_FIT", "Guide star fit", "%5.2f", 0, 1024, 0, 0);
    IUFillNumberVector(&PrimaryCCD.RapidGuideDataNP, PrimaryCCD.RapidGuideDataN, 3, getDeviceName(),
                       "CCD_RAPID_GUIDE_DATA", "Rapid Guide Data", RAPIDGUIDE_TAB, IP_RO, 60, IPS_IDLE);
#endif

    /**********************************************/
    /***************** Guide Chip *****************/
    /**********************************************/

    IUFillNumber(&GuideCCD.ImageFrameN[CCDChip::FRAME_X], "X", "Left ", "%4.0f", 0, 0, 0, 0);
    IUFillNumber(&GuideCCD.ImageFrameN[CCDChip::FRAME_Y], "Y", "Top", "%4.0f", 0, 0, 0, 0);
    IUFillNumber(&GuideCCD.ImageFrameN[CCDChip::FRAME_W], "WIDTH", "Width", "%4.0f", 0, 0, 0, 0);
    IUFillNumber(&GuideCCD.ImageFrameN[CCDChip::FRAME_H], "HEIGHT", "Height", "%4.0f", 0, 0, 0, 0);
    IUFillNumberVector(&GuideCCD.ImageFrameNP, GuideCCD.ImageFrameN, 4, getDeviceName(), "GUIDER_FRAME", "Frame",
                       GUIDE_HEAD_TAB, IP_RW, 60, IPS_IDLE);

    IUFillNumber(&GuideCCD.ImageBinN[0], "HOR_BIN", "X", "%2.0f", 1, 4, 1, 1);
    IUFillNumber(&GuideCCD.ImageBinN[1], "VER_BIN", "Y", "%2.0f", 1, 4, 1, 1);
    IUFillNumberVector(&GuideCCD.ImageBinNP, GuideCCD.ImageBinN, 2, getDeviceName(), "GUIDER_BINNING", "Binning",
                       GUIDE_HEAD_TAB, IP_RW, 60, IPS_IDLE);

    IUFillNumber(&GuideCCD.ImagePixelSizeN[CCDChip::CCD_MAX_X], "CCD_MAX_X", "Max. Width", "%4.0f", 1, 16000, 0, 0);
    IUFillNumber(&GuideCCD.ImagePixelSizeN[CCDChip::CCD_MAX_Y], "CCD_MAX_Y", "Max. Height", "%4.0f", 1, 16000, 0, 0);
    IUFillNumber(&GuideCCD.ImagePixelSizeN[CCDChip::CCD_PIXEL_SIZE], "CCD_PIXEL_SIZE", "Pixel size (um)", "%5.2f", 1,
                 40, 0, 0);
    IUFillNumber(&GuideCCD.ImagePixelSizeN[CCDChip::CCD_PIXEL_SIZE_X], "CCD_PIXEL_SIZE_X", "Pixel size X", "%5.2f", 1,
                 40, 0, 0);
    IUFillNumber(&GuideCCD.ImagePixelSizeN[CCDChip::CCD_PIXEL_SIZE_Y], "CCD_PIXEL_SIZE_Y", "Pixel size Y", "%5.2f", 1,
                 40, 0, 0);
    IUFillNumber(&GuideCCD.ImagePixelSizeN[CCDChip::CCD_BITSPERPIXEL], "CCD_BITSPERPIXEL", "Bits per pixel", "%3.0f", 8,
                 64, 0, 0);
    IUFillNumberVector(&GuideCCD.ImagePixelSizeNP, GuideCCD.ImagePixelSizeN, 6, getDeviceName(), "GUIDER_INFO",
                       "Info", IMAGE_INFO_TAB, IP_RO, 60, IPS_IDLE);

    IUFillSwitch(&GuideCCD.FrameTypeS[0], "FRAME_LIGHT", "Light", ISS_ON);
    IUFillSwitch(&GuideCCD.FrameTypeS[1], "FRAME_BIAS", "Bias", ISS_OFF);
    IUFillSwitch(&GuideCCD.FrameTypeS[2], "FRAME_DARK", "Dark", ISS_OFF);
    IUFillSwitch(&GuideCCD.FrameTypeS[3], "FRAME_FLAT", "Flat", ISS_OFF);
    IUFillSwitchVector(&GuideCCD.FrameTypeSP, GuideCCD.FrameTypeS, 4, getDeviceName(), "GUIDER_FRAME_TYPE",
                       "Type", GUIDE_HEAD_TAB, IP_RW, ISR_1OFMANY, 60, IPS_IDLE);

    IUFillNumber(&GuideCCD.ImageExposureN[0], "GUIDER_EXPOSURE_VALUE", "Duration (s)", "%5.2f", 0.01, 3600, 1.0, 1.0);
    IUFillNumberVector(&GuideCCD.ImageExposureNP, GuideCCD.ImageExposureN, 1, getDeviceName(), "GUIDER_EXPOSURE",
                       "Guide Head", MAIN_CONTROL_TAB, IP_RW, 60, IPS_IDLE);

    IUFillSwitch(&GuideCCD.AbortExposureS[0], "ABORT", "Abort", ISS_OFF);
    IUFillSwitchVector(&GuideCCD.AbortExposureSP, GuideCCD.AbortExposureS, 1, getDeviceName(), "GUIDER_ABORT_EXPOSURE",
                       "Abort", MAIN_CONTROL_TAB, IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    IUFillSwitch(&GuideCCD.CompressS[INDI_ENABLED], "INDI_ENABLED", "Enabled", ISS_OFF);
    IUFillSwitch(&GuideCCD.CompressS[INDI_DISABLED], "INDI_DISABLED", "Disabled", ISS_ON);
    IUFillSwitchVector(&GuideCCD.CompressSP, GuideCCD.CompressS, 2, getDeviceName(), "GUIDER_COMPRESSION", "Compression",
                       GUIDE_HEAD_TAB, IP_RW, ISR_1OFMANY, 60, IPS_IDLE);
    GuideCCD.SendCompressed = false;

    IUFillBLOB(&GuideCCD.FitsB, "CCD2", "Guider Image", "");
    IUFillBLOBVector(&GuideCCD.FitsBP, &GuideCCD.FitsB, 1, getDeviceName(), "CCD2", "Image Data", IMAGE_INFO_TAB, IP_RO,
                     60, IPS_IDLE);

    /**********************************************/
    /********* Guider Chip Rapid Guide  ***********/
    /**********************************************/

#if 0
    IUFillSwitch(&GuideCCD.RapidGuideS[0], "ENABLE", "Enable", ISS_OFF);
    IUFillSwitch(&GuideCCD.RapidGuideS[1], "DISABLE", "Disable", ISS_ON);
    IUFillSwitchVector(&GuideCCD.RapidGuideSP, GuideCCD.RapidGuideS, 2, getDeviceName(), "GUIDER_RAPID_GUIDE",
                       "Guider Head Rapid Guide", OPTIONS_TAB, IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

    IUFillSwitch(&GuideCCD.RapidGuideSetupS[0], "AUTO_LOOP", "Auto loop", ISS_ON);
    IUFillSwitch(&GuideCCD.RapidGuideSetupS[1], "SEND_IMAGE", "Send image", ISS_OFF);
    IUFillSwitch(&GuideCCD.RapidGuideSetupS[2], "SHOW_MARKER", "Show marker", ISS_OFF);
    IUFillSwitchVector(&GuideCCD.RapidGuideSetupSP, GuideCCD.RapidGuideSetupS, 3, getDeviceName(),
                       "GUIDER_RAPID_GUIDE_SETUP", "Rapid Guide Setup", RAPIDGUIDE_TAB, IP_RW, ISR_NOFMANY, 0,
                       IPS_IDLE);

    IUFillNumber(&GuideCCD.RapidGuideDataN[0], "GUIDESTAR_X", "Guide star position X", "%5.2f", 0, 1024, 0, 0);
    IUFillNumber(&GuideCCD.RapidGuideDataN[1], "GUIDESTAR_Y", "Guide star position Y", "%5.2f", 0, 1024, 0, 0);
    IUFillNumber(&GuideCCD.RapidGuideDataN[2], "GUIDESTAR_FIT", "Guide star fit", "%5.2f", 0, 1024, 0, 0);
    IUFillNumberVector(&GuideCCD.RapidGuideDataNP, GuideCCD.RapidGuideDataN, 3, getDeviceName(),
                       "GUIDER_RAPID_GUIDE_DATA", "Rapid Guide Data", RAPIDGUIDE_TAB, IP_RO, 60, IPS_IDLE);

#endif

    /**********************************************/
    /******************** WCS *********************/
    /**********************************************/

    // WCS Enable/Disable
    IUFillSwitch(&WorldCoordS[0], "WCS_ENABLE", "Enable", ISS_OFF);
    IUFillSwitch(&WorldCoordS[1], "WCS_DISABLE", "Disable", ISS_ON);
    IUFillSwitchVector(&WorldCoordSP, WorldCoordS, 2, getDeviceName(), "WCS_CONTROL", "WCS", WCS_TAB, IP_RW,
                       ISR_1OFMANY, 0, IPS_IDLE);

    IUFillNumber(&CCDRotationN[0], "CCD_ROTATION_VALUE", "Rotation", "%g", -360, 360, 1, 0);
    IUFillNumberVector(&CCDRotationNP, CCDRotationN, 1, getDeviceName(), "CCD_ROTATION", "CCD FOV", WCS_TAB, IP_RW, 60,
                       IPS_IDLE);

    IUFillSwitch(&TelescopeTypeS[TELESCOPE_PRIMARY], "TELESCOPE_PRIMARY", "Primary", ISS_ON);
    IUFillSwitch(&TelescopeTypeS[TELESCOPE_GUIDE], "TELESCOPE_GUIDE", "Guide", ISS_OFF);
    IUFillSwitchVector(&TelescopeTypeSP, TelescopeTypeS, 2, getDeviceName(), "TELESCOPE_TYPE", "Telescope", OPTIONS_TAB,
                       IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

    /**********************************************/
    /************** Capture Format ***************/
    /**********************************************/
    char configLabel[64] = {0};
    if (IUGetConfigOnSwitchLabel(getDeviceName(), "CCD_CAPTURE_FORMAT", configLabel, 64) == 0)
        m_ConfigCaptureFormatLabel = configLabel;
    CaptureFormatSP.fill(getDeviceName(), "CCD_CAPTURE_FORMAT", "Format", IMAGE_SETTINGS_TAB, IP_RW, ISR_1OFMANY, 60,
                         IPS_IDLE);

    m_ConfigEncodeFormatIndex = FORMAT_FITS;
    IUGetConfigOnSwitchIndex(getDeviceName(), "CCD_TRANSFER_FORMAT", &m_ConfigEncodeFormatIndex);
    EncodeFormatSP[FORMAT_FITS].fill("FORMAT_FITS", "FITS",
                                     m_ConfigEncodeFormatIndex == FORMAT_FITS ? ISS_ON : ISS_OFF);
    EncodeFormatSP[FORMAT_NATIVE].fill("FORMAT_NATIVE", "Native",
                                       m_ConfigEncodeFormatIndex == FORMAT_NATIVE ? ISS_ON : ISS_OFF);
    EncodeFormatSP.fill(getDeviceName(), "CCD_TRANSFER_FORMAT", "Encode", IMAGE_SETTINGS_TAB, IP_RW, ISR_1OFMANY, 60,
                        IPS_IDLE);

    /**********************************************/
    /************** Upload Settings ***************/
    /**********************************************/

    // Upload Mode
    IUFillSwitch(&UploadS[UPLOAD_CLIENT], "UPLOAD_CLIENT", "Client", ISS_ON);
    IUFillSwitch(&UploadS[UPLOAD_LOCAL], "UPLOAD_LOCAL", "Local", ISS_OFF);
    IUFillSwitch(&UploadS[UPLOAD_BOTH], "UPLOAD_BOTH", "Both", ISS_OFF);
    IUFillSwitchVector(&UploadSP, UploadS, 3, getDeviceName(), "UPLOAD_MODE", "Upload", OPTIONS_TAB, IP_RW, ISR_1OFMANY,
                       0, IPS_IDLE);

    // Upload Settings
    IUFillText(&UploadSettingsT[UPLOAD_DIR], "UPLOAD_DIR", "Dir", "");
    IUFillText(&UploadSettingsT[UPLOAD_PREFIX], "UPLOAD_PREFIX", "Prefix", "IMAGE_XXX");
    IUFillTextVector(&UploadSettingsTP, UploadSettingsT, 2, getDeviceName(), "UPLOAD_SETTINGS", "Upload Settings",
                     OPTIONS_TAB, IP_RW, 60, IPS_IDLE);

    // Upload File Path
    IUFillText(&FileNameT[0], "FILE_PATH", "Path", "");
    IUFillTextVector(&FileNameTP, FileNameT, 1, getDeviceName(), "CCD_FILE_PATH", "Filename", IMAGE_INFO_TAB, IP_RO, 60,
                     IPS_IDLE);

    /**********************************************/
    /****************** FITS Header****************/
    /**********************************************/

    IUFillText(&FITSHeaderT[FITS_OBSERVER], "FITS_OBSERVER", "Observer", "Unknown");
    IUFillText(&FITSHeaderT[FITS_OBJECT], "FITS_OBJECT", "Object", "Unknown");
    IUFillTextVector(&FITSHeaderTP, FITSHeaderT, 2, getDeviceName(), "FITS_HEADER", "FITS Header", INFO_TAB, IP_RW, 60,
                     IPS_IDLE);

    /**********************************************/
    /****************** Exposure Looping **********/
    /***************** Primary CCD Only ***********/
    IUGetConfigOnSwitchIndex(getDeviceName(), FastExposureToggleSP.name, &m_ConfigFastExposureIndex);
    IUFillSwitch(&FastExposureToggleS[INDI_ENABLED], "INDI_ENABLED", "Enabled",
                 m_ConfigFastExposureIndex == INDI_ENABLED ? ISS_ON : ISS_OFF);
    IUFillSwitch(&FastExposureToggleS[INDI_DISABLED], "INDI_DISABLED", "Disabled",
                 m_ConfigFastExposureIndex == INDI_DISABLED ? ISS_ON : ISS_OFF);
    IUFillSwitchVector(&FastExposureToggleSP, FastExposureToggleS, 2, getDeviceName(), "CCD_FAST_TOGGLE", "Fast Exposure",
                       OPTIONS_TAB, IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

    // CCD Should loop until the number of frames specified in this property is completed
    IUFillNumber(&FastExposureCountN[0], "FRAMES", "Frames", "%.f", 0, 100000, 1, 1);
    IUFillNumberVector(&FastExposureCountNP, FastExposureCountN, 1, getDeviceName(), "CCD_FAST_COUNT", "Fast Count",
                       OPTIONS_TAB, IP_RW, 0, IPS_IDLE);

    /**********************************************/
    /**************** Web Socket ******************/
    /**********************************************/
    IUFillSwitch(&WebSocketS[WEBSOCKET_ENABLED], "WEBSOCKET_ENABLED", "Enabled", ISS_OFF);
    IUFillSwitch(&WebSocketS[WEBSOCKET_DISABLED], "WEBSOCKET_DISABLED", "Disabled", ISS_ON);
    IUFillSwitchVector(&WebSocketSP, WebSocketS, 2, getDeviceName(), "CCD_WEBSOCKET", "Websocket", OPTIONS_TAB,
                       IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

    IUFillNumber(&WebSocketSettingsN[WS_SETTINGS_PORT], "WS_SETTINGS_PORT", "Port", "%.f", 0, 50000, 0, 0);
    IUFillNumberVector(&WebSocketSettingsNP, WebSocketSettingsN, 1, getDeviceName(), "CCD_WEBSOCKET_SETTINGS", "WS Settings",
                       OPTIONS_TAB, IP_RW,
                       60, IPS_IDLE);

    /**********************************************/
    /**************** Snooping ********************/
    /**********************************************/

    // Snooped Devices

    IUFillText(&ActiveDeviceT[ACTIVE_TELESCOPE], "ACTIVE_TELESCOPE", "Telescope", "Telescope Simulator");
    IUFillText(&ActiveDeviceT[ACTIVE_ROTATOR], "ACTIVE_ROTATOR", "Rotator", "Rotator Simulator");
    IUFillText(&ActiveDeviceT[ACTIVE_FOCUSER], "ACTIVE_FOCUSER", "Focuser", "Focuser Simulator");
    IUFillText(&ActiveDeviceT[ACTIVE_FILTER], "ACTIVE_FILTER", "Filter", "CCD Simulator");
    IUFillText(&ActiveDeviceT[ACTIVE_SKYQUALITY], "ACTIVE_SKYQUALITY", "Sky Quality", "SQM");
    IUFillTextVector(&ActiveDeviceTP, ActiveDeviceT, 5, getDeviceName(), "ACTIVE_DEVICES", "Snoop devices", OPTIONS_TAB,
                     IP_RW, 60, IPS_IDLE);

    // Snooped RA/DEC Property
    IUFillNumber(&EqN[0], "RA", "Ra (hh:mm:ss)", "%010.6m", 0, 24, 0, 0);
    IUFillNumber(&EqN[1], "DEC", "Dec (dd:mm:ss)", "%010.6m", -90, 90, 0, 0);
    IUFillNumberVector(&EqNP, EqN, 2, ActiveDeviceT[ACTIVE_TELESCOPE].text, "EQUATORIAL_EOD_COORD", "EQ Coord", "Main Control",
                       IP_RW,
                       60, IPS_IDLE);

    // Snooped J2000 RA/DEC Property
    IUFillNumber(&J2000EqN[0], "RA", "Ra (hh:mm:ss)", "%010.6m", 0, 24, 0, 0);
    IUFillNumber(&J2000EqN[1], "DEC", "Dec (dd:mm:ss)", "%010.6m", -90, 90, 0, 0);
    IUFillNumberVector(&J2000EqNP, J2000EqN, 2, ActiveDeviceT[ACTIVE_TELESCOPE].text, "EQUATORIAL_COORD", "J2000 EQ Coord",
                       "Main Control", IP_RW,
                       60, IPS_IDLE);

    // Snoop properties of interest

    // Snoop mount
    IDSnoopDevice(ActiveDeviceT[ACTIVE_TELESCOPE].text, "EQUATORIAL_EOD_COORD");
    IDSnoopDevice(ActiveDeviceT[ACTIVE_TELESCOPE].text, "EQUATORIAL_COORD");
    IDSnoopDevice(ActiveDeviceT[ACTIVE_TELESCOPE].text, "TELESCOPE_INFO");
    IDSnoopDevice(ActiveDeviceT[ACTIVE_TELESCOPE].text, "GEOGRAPHIC_COORD");
    IDSnoopDevice(ActiveDeviceT[ACTIVE_TELESCOPE].text, "TELESCOPE_PIER_SIDE");

    // Snoop Rotator
    IDSnoopDevice(ActiveDeviceT[ACTIVE_ROTATOR].text, "ABS_ROTATOR_ANGLE");

    // JJ ed 2019-12-10
    // Snoop Focuser
    IDSnoopDevice(ActiveDeviceT[ACTIVE_FOCUSER].text, "ABS_FOCUS_POSITION");
    IDSnoopDevice(ActiveDeviceT[ACTIVE_FOCUSER].text, "FOCUS_TEMPERATURE");
    //

    // Snoop Filter Wheel
    IDSnoopDevice(ActiveDeviceT[ACTIVE_FILTER].text, "FILTER_SLOT");
    IDSnoopDevice(ActiveDeviceT[ACTIVE_FILTER].text, "FILTER_NAME");

    // Snoop Sky Quality Meter
    IDSnoopDevice(ActiveDeviceT[ACTIVE_SKYQUALITY].text, "SKY_QUALITY");

    // Guider Interface
    initGuiderProperties(getDeviceName(), GUIDE_CONTROL_TAB);

    addPollPeriodControl();

    setDriverInterface(CCD_INTERFACE | GUIDER_INTERFACE);

    return true;
}

void CCD::ISGetProperties(const char * dev)
{
    DefaultDevice::ISGetProperties(dev);

    defineProperty(&ActiveDeviceTP);
    loadConfig(true, "ACTIVE_DEVICES");

    if (HasStreaming())
        Streamer->ISGetProperties(dev);

    if (HasDSP())
        DSP->ISGetProperties(dev);
}

bool CCD::updateProperties()
{
    //IDLog("CCD UpdateProperties isConnected returns %d %d\n",isConnected(),Connected);
    if (isConnected())
    {
        defineProperty(&PrimaryCCD.ImageExposureNP);

        if (CanAbort())
            defineProperty(&PrimaryCCD.AbortExposureSP);
        if (CanSubFrame() == false)
            PrimaryCCD.ImageFrameNP.p = IP_RO;

        defineProperty(&PrimaryCCD.ImageFrameNP);
        if (CanBin() || CanSubFrame())
            defineProperty(&PrimaryCCD.ResetSP);

        if (CanBin())
            defineProperty(&PrimaryCCD.ImageBinNP);

        defineProperty(&FITSHeaderTP);

        if (HasGuideHead())
        {
            defineProperty(&GuideCCD.ImageExposureNP);
            if (CanAbort())
                defineProperty(&GuideCCD.AbortExposureSP);
            if (CanSubFrame() == false)
                GuideCCD.ImageFrameNP.p = IP_RO;
            defineProperty(&GuideCCD.ImageFrameNP);
        }

        if (HasCooler())
        {
            defineProperty(&TemperatureNP);
            defineProperty(&TemperatureRampNP);
        }

        defineProperty(&CaptureFormatSP);
        defineProperty(&EncodeFormatSP);

        defineProperty(&PrimaryCCD.ImagePixelSizeNP);
        if (HasGuideHead())
        {
            defineProperty(&GuideCCD.ImagePixelSizeNP);
            if (CanBin())
                defineProperty(&GuideCCD.ImageBinNP);
        }
        defineProperty(&PrimaryCCD.CompressSP);
        defineProperty(&PrimaryCCD.FitsBP);
        if (HasGuideHead())
        {
            defineProperty(&GuideCCD.CompressSP);
            defineProperty(&GuideCCD.FitsBP);
        }
        if (HasST4Port())
        {
            defineProperty(&GuideNSNP);
            defineProperty(&GuideWENP);
        }
        defineProperty(&PrimaryCCD.FrameTypeSP);

        if (HasGuideHead())
            defineProperty(&GuideCCD.FrameTypeSP);

        if (HasBayer())
            defineProperty(&BayerTP);

#if 0
        defineProperty(&PrimaryCCD.RapidGuideSP);

        if (HasGuideHead())
            defineProperty(&GuideCCD.RapidGuideSP);

        if (RapidGuideEnabled)
        {
            defineProperty(&PrimaryCCD.RapidGuideSetupSP);
            defineProperty(&PrimaryCCD.RapidGuideDataNP);
        }
        if (GuiderRapidGuideEnabled)
        {
            defineProperty(&GuideCCD.RapidGuideSetupSP);
            defineProperty(&GuideCCD.RapidGuideDataNP);
        }
#endif
        defineProperty(&TelescopeTypeSP);

        defineProperty(&WorldCoordSP);
        defineProperty(&UploadSP);

        if (UploadSettingsT[UPLOAD_DIR].text == nullptr)
            IUSaveText(&UploadSettingsT[UPLOAD_DIR], getenv("HOME"));
        defineProperty(&UploadSettingsTP);

#ifdef HAVE_WEBSOCKET
        if (HasWebSocket())
            defineProperty(&WebSocketSP);
#endif

        defineProperty(&FastExposureToggleSP);
        defineProperty(&FastExposureCountNP);
    }
    else
    {
        deleteProperty(PrimaryCCD.ImageFrameNP.name);
        if (CanBin() || CanSubFrame())
            deleteProperty(PrimaryCCD.ResetSP.name);

        deleteProperty(PrimaryCCD.ImagePixelSizeNP.name);

        deleteProperty(CaptureFormatSP.getName());
        deleteProperty(EncodeFormatSP.getName());

        if (CanBin())
            deleteProperty(PrimaryCCD.ImageBinNP.name);

        deleteProperty(PrimaryCCD.ImageExposureNP.name);
        if (CanAbort())
            deleteProperty(PrimaryCCD.AbortExposureSP.name);
        deleteProperty(PrimaryCCD.FitsBP.name);
        deleteProperty(PrimaryCCD.CompressSP.name);

#if 0
        deleteProperty(PrimaryCCD.RapidGuideSP.name);
        if (RapidGuideEnabled)
        {
            deleteProperty(PrimaryCCD.RapidGuideSetupSP.name);
            deleteProperty(PrimaryCCD.RapidGuideDataNP.name);
        }
#endif

        deleteProperty(FITSHeaderTP.name);

        if (HasGuideHead())
        {
            deleteProperty(GuideCCD.ImageExposureNP.name);
            if (CanAbort())
                deleteProperty(GuideCCD.AbortExposureSP.name);
            deleteProperty(GuideCCD.ImageFrameNP.name);
            deleteProperty(GuideCCD.ImagePixelSizeNP.name);

            deleteProperty(GuideCCD.FitsBP.name);
            if (CanBin())
                deleteProperty(GuideCCD.ImageBinNP.name);
            deleteProperty(GuideCCD.CompressSP.name);
            deleteProperty(GuideCCD.FrameTypeSP.name);

#if 0
            deleteProperty(GuideCCD.RapidGuideSP.name);
            if (GuiderRapidGuideEnabled)
            {
                deleteProperty(GuideCCD.RapidGuideSetupSP.name);
                deleteProperty(GuideCCD.RapidGuideDataNP.name);
            }
#endif
        }
        if (HasCooler())
        {
            deleteProperty(TemperatureNP.name);
            deleteProperty(TemperatureRampNP.getName());
        }
        if (HasST4Port())
        {
            deleteProperty(GuideNSNP.name);
            deleteProperty(GuideWENP.name);
        }
        deleteProperty(PrimaryCCD.FrameTypeSP.name);
        if (HasBayer())
            deleteProperty(BayerTP.name);
        deleteProperty(TelescopeTypeSP.name);

        if (WorldCoordS[0].s == ISS_ON)
        {
            deleteProperty(CCDRotationNP.name);
        }
        deleteProperty(WorldCoordSP.name);
        deleteProperty(UploadSP.name);
        deleteProperty(UploadSettingsTP.name);

#ifdef HAVE_WEBSOCKET
        if (HasWebSocket())
        {
            deleteProperty(WebSocketSP.name);
            deleteProperty(WebSocketSettingsNP.name);
        }
#endif
        deleteProperty(FastExposureToggleSP.name);
        deleteProperty(FastExposureCountNP.name);
    }

    // Streamer
    if (HasStreaming())
        Streamer->updateProperties();

    // DSP
    if (HasDSP())
        DSP->updateProperties();

    return true;
}

bool CCD::ISSnoopDevice(XMLEle * root)
{
    XMLEle * ep           = nullptr;
    const char * propName = findXMLAttValu(root, "name");

    if (IUSnoopNumber(root, &EqNP) == 0)
    {
        double newra, newdec;
        newra  = EqN[0].value;
        newdec = EqN[1].value;
        if ((newra != RA) || (newdec != Dec))
        {
            //IDLog("RA %4.2f  Dec %4.2f Snooped RA %4.2f  Dec %4.2f\n",RA,Dec,newra,newdec);
            RA  = newra;
            Dec = newdec;
        }
    }
    else if (IUSnoopNumber(root, &J2000EqNP) == 0)
    {
        float newra, newdec;
        newra  = J2000EqN[0].value;
        newdec = J2000EqN[1].value;
        if ((newra != J2000RA) || (newdec != J2000DE))
        {
            //    	    IDLog("J2000 RA %4.2f  Dec %4.2f Snooped RA %4.2f  Dec %4.2f\n",J2000RA,J2000DE,newra,newdec);
            J2000RA = newra;
            J2000DE = newdec;
        }
        J2000Valid = true;
    }
    else if (!strcmp("TELESCOPE_PIER_SIDE", propName))
    {
        // set default to say we have no valid information from mount
        pierSide = -1;
        //  crack the message
        for (ep = nextXMLEle(root, 1); ep != nullptr; ep = nextXMLEle(root, 0))
        {
            const char * elemName = findXMLAttValu(ep, "name");

            if (!strcmp(elemName, "PIER_EAST") && !strcmp(pcdataXMLEle(ep), "On"))
                pierSide = 1;
            else if (!strcmp(elemName, "PIER_WEST") && !strcmp(pcdataXMLEle(ep), "On"))
                pierSide = 0;
        }
    }
    else if (!strcmp(propName, "TELESCOPE_INFO"))
    {
        for (ep = nextXMLEle(root, 1); ep != nullptr; ep = nextXMLEle(root, 0))
        {
            const char * name = findXMLAttValu(ep, "name");

            if (!strcmp(name, "TELESCOPE_APERTURE"))
            {
                primaryAperture = atof(pcdataXMLEle(ep));
            }
            else if (!strcmp(name, "TELESCOPE_FOCAL_LENGTH"))
            {
                primaryFocalLength = atof(pcdataXMLEle(ep));
            }
            else if (!strcmp(name, "GUIDER_APERTURE"))
            {
                guiderAperture = atof(pcdataXMLEle(ep));
            }
            else if (!strcmp(name, "GUIDER_FOCAL_LENGTH"))
            {
                guiderFocalLength = atof(pcdataXMLEle(ep));
            }
        }
    }
    else if (!strcmp(propName, "FILTER_NAME"))
    {
        FilterNames.clear();

        for (ep = nextXMLEle(root, 1); ep != nullptr; ep = nextXMLEle(root, 0))
            FilterNames.push_back(pcdataXMLEle(ep));
    }
    else if (!strcmp(propName, "FILTER_SLOT"))
    {
        CurrentFilterSlot = -1;
        for (ep = nextXMLEle(root, 1); ep != nullptr; ep = nextXMLEle(root, 0))
            CurrentFilterSlot = atoi(pcdataXMLEle(ep));
    }
    else if (!strcmp(propName, "SKY_QUALITY"))
    {
        for (ep = nextXMLEle(root, 1); ep != nullptr; ep = nextXMLEle(root, 0))
        {
            const char * name = findXMLAttValu(ep, "name");

            if (!strcmp(name, "SKY_BRIGHTNESS"))
            {
                MPSAS = atof(pcdataXMLEle(ep));
                break;
            }
        }
    }
    else if (!strcmp(propName, "ABS_ROTATOR_ANGLE"))
    {
        for (ep = nextXMLEle(root, 1); ep != nullptr; ep = nextXMLEle(root, 0))
        {
            const char * name = findXMLAttValu(ep, "name");

            if (!strcmp(name, "ANGLE"))
            {
                RotatorAngle = atof(pcdataXMLEle(ep));
                break;
            }
        }
    }

    // JJ ed 2019-12-10
    else if (!strcmp(propName, "ABS_FOCUS_POSITION"))
    {
        for (ep = nextXMLEle(root, 1); ep != nullptr; ep = nextXMLEle(root, 0))
        {
            const char * name = findXMLAttValu(ep, "name");

            if (!strcmp(name, "FOCUS_ABSOLUTE_POSITION"))
            {
                FocuserPos = atol(pcdataXMLEle(ep));
                break;
            }
        }
    }
    else if (!strcmp(propName, "FOCUS_TEMPERATURE"))
    {
        for (ep = nextXMLEle(root, 1); ep != nullptr; ep = nextXMLEle(root, 0))
        {
            const char * name = findXMLAttValu(ep, "name");

            if (!strcmp(name, "TEMPERATURE"))
            {
                FocuserTemp = atof(pcdataXMLEle(ep));
                break;
            }
        }
    }
    //

    else if (!strcmp(propName, "GEOGRAPHIC_COORD"))
    {
        for (ep = nextXMLEle(root, 1); ep != nullptr; ep = nextXMLEle(root, 0))
        {
            const char * name = findXMLAttValu(ep, "name");

            if (!strcmp(name, "LONG"))
            {
                Longitude = atof(pcdataXMLEle(ep));
                if (Longitude > 180)
                    Longitude -= 360;
            }
            else if (!strcmp(name, "LAT"))
            {
                Latitude = atof(pcdataXMLEle(ep));
            }
        }
    }

    return DefaultDevice::ISSnoopDevice(root);
}

bool CCD::ISNewText(const char * dev, const char * name, char * texts[], char * names[], int n)
{
    //  first check if it's for our device
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        //  This is for our device
        //  Now lets see if it's something we process here
        if (!strcmp(name, ActiveDeviceTP.name))
        {
            ActiveDeviceTP.s = IPS_OK;
            IUUpdateText(&ActiveDeviceTP, texts, names, n);
            IDSetText(&ActiveDeviceTP, nullptr);

            // Update the property name!
            strncpy(EqNP.device, ActiveDeviceT[ACTIVE_TELESCOPE].text, MAXINDIDEVICE);
            strncpy(J2000EqNP.device, ActiveDeviceT[ACTIVE_TELESCOPE].text, MAXINDIDEVICE);
            if (strlen(ActiveDeviceT[ACTIVE_TELESCOPE].text) > 0)
            {
                IDSnoopDevice(ActiveDeviceT[ACTIVE_TELESCOPE].text, "EQUATORIAL_EOD_COORD");
                IDSnoopDevice(ActiveDeviceT[ACTIVE_TELESCOPE].text, "EQUATORIAL_COORD");
                IDSnoopDevice(ActiveDeviceT[ACTIVE_TELESCOPE].text, "TELESCOPE_INFO");
                IDSnoopDevice(ActiveDeviceT[ACTIVE_TELESCOPE].text, "GEOGRAPHIC_COORD");
            }
            else
            {
                RA = std::numeric_limits<double>::quiet_NaN();
                Dec = std::numeric_limits<double>::quiet_NaN();
                J2000RA = std::numeric_limits<double>::quiet_NaN();
                J2000DE = std::numeric_limits<double>::quiet_NaN();
                Latitude = std::numeric_limits<double>::quiet_NaN();
                Longitude = std::numeric_limits<double>::quiet_NaN();
                Airmass = std::numeric_limits<double>::quiet_NaN();
                Azimuth = std::numeric_limits<double>::quiet_NaN();
                Altitude = std::numeric_limits<double>::quiet_NaN();
            }

            if (strlen(ActiveDeviceT[ACTIVE_ROTATOR].text) > 0)
                IDSnoopDevice(ActiveDeviceT[ACTIVE_ROTATOR].text, "ABS_ROTATOR_ANGLE");
            else
                MPSAS = std::numeric_limits<double>::quiet_NaN();

            // JJ ed 2019-12-10
            if (strlen(ActiveDeviceT[ACTIVE_FOCUSER].text) > 0)
            {
                IDSnoopDevice(ActiveDeviceT[ACTIVE_FOCUSER].text, "ABS_FOCUS_POSITION");
                IDSnoopDevice(ActiveDeviceT[ACTIVE_FOCUSER].text, "FOCUS_TEMPERATURE");
            }
            else
            {
                FocuserPos = -1;
                FocuserTemp = std::numeric_limits<double>::quiet_NaN();
            }
            //


            if (strlen(ActiveDeviceT[ACTIVE_FILTER].text) > 0)
            {
                IDSnoopDevice(ActiveDeviceT[ACTIVE_FILTER].text, "FILTER_SLOT");
                IDSnoopDevice(ActiveDeviceT[ACTIVE_FILTER].text, "FILTER_NAME");
            }
            else
            {
                CurrentFilterSlot = -1;
            }

            IDSnoopDevice(ActiveDeviceT[ACTIVE_SKYQUALITY].text, "SKY_QUALITY");

            // Tell children active devices was updated.
            activeDevicesUpdated();

            //  We processed this one, so, tell the world we did it
            return true;
        }

        if (!strcmp(name, BayerTP.name))
        {
            IUUpdateText(&BayerTP, texts, names, n);
            BayerTP.s = IPS_OK;
            IDSetText(&BayerTP, nullptr);
            return true;
        }

        if (!strcmp(name, FITSHeaderTP.name))
        {
            IUUpdateText(&FITSHeaderTP, texts, names, n);
            FITSHeaderTP.s = IPS_OK;
            IDSetText(&FITSHeaderTP, nullptr);
            return true;
        }

        if (!strcmp(name, UploadSettingsTP.name))
        {
            IUUpdateText(&UploadSettingsTP, texts, names, n);
            UploadSettingsTP.s = IPS_OK;
            IDSetText(&UploadSettingsTP, nullptr);
            return true;
        }
    }

    // Streamer
    if (HasStreaming())
        Streamer->ISNewText(dev, name, texts, names, n);

    // DSP
    if (HasDSP())
        DSP->ISNewText(dev, name, texts, names, n);

    return DefaultDevice::ISNewText(dev, name, texts, names, n);
}

bool CCD::ISNewNumber(const char * dev, const char * name, double values[], char * names[], int n)
{
    //  first check if it's for our device
    //IDLog("CCD::ISNewNumber %s\n",name);
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        if (!strcmp(name, "CCD_EXPOSURE"))
        {
            if (PrimaryCCD.getFrameType() != CCDChip::BIAS_FRAME &&
                    (values[0] < PrimaryCCD.ImageExposureN[0].min || values[0] > PrimaryCCD.ImageExposureN[0].max))
            {
                LOGF_ERROR("Requested exposure value (%g) seconds out of bounds [%g,%g].",
                           values[0], PrimaryCCD.ImageExposureN[0].min, PrimaryCCD.ImageExposureN[0].max);
                PrimaryCCD.ImageExposureNP.s = IPS_ALERT;
                IDSetNumber(&PrimaryCCD.ImageExposureNP, nullptr);
                return false;
            }

            if (PrimaryCCD.getFrameType() == CCDChip::BIAS_FRAME)
                PrimaryCCD.ImageExposureN[0].value = ExposureTime = PrimaryCCD.ImageExposureN[0].min;
            else
                PrimaryCCD.ImageExposureN[0].value = ExposureTime = values[0];

            // Only abort when busy if we are not already in an exposure loops
            //if (PrimaryCCD.ImageExposureNP.s == IPS_BUSY && FastExposureToggleS[INDI_DISABLED].s == ISS_ON)
            if (PrimaryCCD.ImageExposureNP.s == IPS_BUSY)
            {
                if (CanAbort() && AbortExposure() == false)
                    DEBUG(Logger::DBG_WARNING, "Warning: Aborting exposure failed.");
            }

            if (StartExposure(ExposureTime))
            {
                PrimaryCCD.ImageExposureNP.s = IPS_BUSY;
                if (ExposureTime * 1000 < getCurrentPollingPeriod())
                    setCurrentPollingPeriod(ExposureTime * 950);
            }
            else
                PrimaryCCD.ImageExposureNP.s = IPS_ALERT;
            IDSetNumber(&PrimaryCCD.ImageExposureNP, nullptr);
            return true;
        }

        if (!strcmp(name, "GUIDER_EXPOSURE"))
        {
            if (GuideCCD.getFrameType() != CCDChip::BIAS_FRAME &&
                    (values[0] < GuideCCD.ImageExposureN[0].min || values[0] > GuideCCD.ImageExposureN[0].max))
            {
                LOGF_ERROR("Requested guide exposure value (%g) seconds out of bounds [%g,%g].",
                           values[0], GuideCCD.ImageExposureN[0].min, GuideCCD.ImageExposureN[0].max);
                GuideCCD.ImageExposureNP.s = IPS_ALERT;
                IDSetNumber(&GuideCCD.ImageExposureNP, nullptr);
                return false;
            }

            if (GuideCCD.getFrameType() == CCDChip::BIAS_FRAME)
                GuideCCD.ImageExposureN[0].value = GuiderExposureTime = GuideCCD.ImageExposureN[0].min;
            else
                GuideCCD.ImageExposureN[0].value = GuiderExposureTime = values[0];

            GuideCCD.ImageExposureNP.s = IPS_BUSY;
            if (StartGuideExposure(GuiderExposureTime))
                GuideCCD.ImageExposureNP.s = IPS_BUSY;
            else
                GuideCCD.ImageExposureNP.s = IPS_ALERT;
            IDSetNumber(&GuideCCD.ImageExposureNP, nullptr);
            return true;
        }

        if (!strcmp(name, "CCD_BINNING"))
        {
            //  We are being asked to set camera binning
            INumber * np = IUFindNumber(&PrimaryCCD.ImageBinNP, names[0]);
            if (np == nullptr)
            {
                PrimaryCCD.ImageBinNP.s = IPS_ALERT;
                IDSetNumber(&PrimaryCCD.ImageBinNP, nullptr);
                return false;
            }

            int binx, biny;
            if (!strcmp(np->name, "HOR_BIN"))
            {
                binx = values[0];
                biny = values[1];
            }
            else
            {
                binx = values[1];
                biny = values[0];
            }

            if (UpdateCCDBin(binx, biny))
            {
                IUUpdateNumber(&PrimaryCCD.ImageBinNP, values, names, n);
                PrimaryCCD.ImageBinNP.s = IPS_OK;
            }
            else
                PrimaryCCD.ImageBinNP.s = IPS_ALERT;

            IDSetNumber(&PrimaryCCD.ImageBinNP, nullptr);

            return true;
        }

        if (!strcmp(name, "GUIDER_BINNING"))
        {
            //  We are being asked to set camera binning
            INumber * np = IUFindNumber(&GuideCCD.ImageBinNP, names[0]);
            if (np == nullptr)
            {
                GuideCCD.ImageBinNP.s = IPS_ALERT;
                IDSetNumber(&GuideCCD.ImageBinNP, nullptr);
                return false;
            }

            int binx, biny;
            if (!strcmp(np->name, "HOR_BIN"))
            {
                binx = values[0];
                biny = values[1];
            }
            else
            {
                binx = values[1];
                biny = values[0];
            }

            if (UpdateGuiderBin(binx, biny))
            {
                IUUpdateNumber(&GuideCCD.ImageBinNP, values, names, n);
                GuideCCD.ImageBinNP.s = IPS_OK;
            }
            else
                GuideCCD.ImageBinNP.s = IPS_ALERT;

            IDSetNumber(&GuideCCD.ImageBinNP, nullptr);

            return true;
        }

        if (!strcmp(name, "CCD_FRAME"))
        {
            int x = -1, y = -1, w = -1, h = -1;
            for (int i = 0; i < n; i++)
            {
                if (!strcmp(names[i], "X"))
                    x = values[i];
                else if (!strcmp(names[i], "Y"))
                    y = values[i];
                else if (!strcmp(names[i], "WIDTH"))
                    w = values[i];
                else if (!strcmp(names[i], "HEIGHT"))
                    h = values[i];
            }

            DEBUGF(Logger::DBG_DEBUG, "Requested CCD Frame is (%d,%d) (%d x %d)", x, y, w, h);

            if (x < 0 || y < 0 || w <= 0 || h <= 0)
            {
                LOGF_ERROR("Invalid frame requested (%d,%d) (%d x %d)", x, y, w, h);
                PrimaryCCD.ImageFrameNP.s = IPS_ALERT;
                IDSetNumber(&PrimaryCCD.ImageFrameNP, nullptr);
                return true;
            }

            if (UpdateCCDFrame(x, y, w, h))
            {
                PrimaryCCD.ImageFrameNP.s = IPS_OK;
                IUUpdateNumber(&PrimaryCCD.ImageFrameNP, values, names, n);
            }
            else
                PrimaryCCD.ImageFrameNP.s = IPS_ALERT;

            IDSetNumber(&PrimaryCCD.ImageFrameNP, nullptr);
            return true;
        }

        if (!strcmp(name, "GUIDER_FRAME"))
        {
            //  We are being asked to set guide frame
            if (IUUpdateNumber(&GuideCCD.ImageFrameNP, values, names, n) < 0)
                return false;

            GuideCCD.ImageFrameNP.s = IPS_OK;

            DEBUGF(Logger::DBG_DEBUG, "Requested Guide Frame is %4.0f,%4.0f %4.0f x %4.0f", values[0], values[1],
                   values[2], values[4]);

            if (UpdateGuiderFrame(GuideCCD.ImageFrameN[0].value, GuideCCD.ImageFrameN[1].value,
                                  GuideCCD.ImageFrameN[2].value, GuideCCD.ImageFrameN[3].value) == false)
                GuideCCD.ImageFrameNP.s = IPS_ALERT;

            IDSetNumber(&GuideCCD.ImageFrameNP, nullptr);

            return true;
        }

#if 0
        if (!strcmp(name, "CCD_GUIDESTAR"))
        {
            PrimaryCCD.RapidGuideDataNP.s = IPS_OK;
            IUUpdateNumber(&PrimaryCCD.RapidGuideDataNP, values, names, n);
            IDSetNumber(&PrimaryCCD.RapidGuideDataNP, nullptr);
            return true;
        }

        if (!strcmp(name, "GUIDER_GUIDESTAR"))
        {
            GuideCCD.RapidGuideDataNP.s = IPS_OK;
            IUUpdateNumber(&GuideCCD.RapidGuideDataNP, values, names, n);
            IDSetNumber(&GuideCCD.RapidGuideDataNP, nullptr);
            return true;
        }
#endif

        if (!strcmp(name, GuideNSNP.name) || !strcmp(name, GuideWENP.name))
        {
            processGuiderProperties(name, values, names, n);
            return true;
        }

        // Fast Exposure Count
        if (!strcmp(name, FastExposureCountNP.name))
        {
            IUUpdateNumber(&FastExposureCountNP, values, names, n);
            FastExposureCountNP.s = IPS_OK;
            IDSetNumber(&FastExposureCountNP, nullptr);
            return true;
        }

        // CCD TEMPERATURE
        if (!strcmp(name, TemperatureNP.name))
        {
            if (values[0] < TemperatureN[0].min || values[0] > TemperatureN[0].max)
            {
                TemperatureNP.s = IPS_ALERT;
                LOGF_ERROR("Error: Bad temperature value! Range is [%.1f, %.1f] [C].",
                           TemperatureN[0].min, TemperatureN[0].max);
                IDSetNumber(&TemperatureNP, nullptr);
                return false;
            }

            double nextTemperature = values[0];
            // If temperature ramp is enabled, find
            if (TemperatureRampNP[RAMP_SLOPE].getValue() != 0)
            {
                if (values[0] < TemperatureN[0].value)
                {
                    nextTemperature = std::max(values[0], TemperatureN[0].value - TemperatureRampNP[RAMP_SLOPE].getValue());
                }
                // Going up
                else
                {
                    nextTemperature = std::min(values[0], TemperatureN[0].value + TemperatureRampNP[RAMP_SLOPE].getValue());
                }
            }

            int rc = SetTemperature(nextTemperature);

            if (rc == 0)
            {
                if (TemperatureRampNP[RAMP_SLOPE].getValue() != 0)
                    m_TemperatureElapsedTimer.start();

                m_TargetTemperature = values[0];
                m_TemperatureCheckTimer.start();
                TemperatureNP.s = IPS_BUSY;
            }
            else if (rc == 1)
                TemperatureNP.s = IPS_OK;
            else
                TemperatureNP.s = IPS_ALERT;

            IDSetNumber(&TemperatureNP, nullptr);
            return true;
        }

        // Camera Temperature Ramp
        if (!strcmp(name, TemperatureRampNP.getName()))
        {
            double previousSlope     = TemperatureRampNP[RAMP_SLOPE].getValue();
            double previousThreshold = TemperatureRampNP[RAMP_THRESHOLD].getValue();
            TemperatureRampNP.update(values, names, n);
            TemperatureRampNP.setState(IPS_OK);
            TemperatureRampNP.apply();
            if (TemperatureRampNP[0].getValue() == 0)
                LOG_INFO("Temperature ramp is disabled.");
            else
                LOGF_INFO("Temperature ramp is enabled. Gradual cooling and warming is regulated at %.f Celsius per minute.",
                          TemperatureRampNP[0].getValue());

            // Save config if there is a change
            if (std::abs(previousSlope - TemperatureRampNP[RAMP_SLOPE].getValue()) > 0 ||
                    std::abs(previousThreshold - TemperatureRampNP[RAMP_THRESHOLD].getValue()) > 0.01)
                saveConfig(true, TemperatureRampNP.getName());
            return true;
        }

        // Primary CCD Info
        if (!strcmp(name, PrimaryCCD.ImagePixelSizeNP.name))
        {
            if (IUUpdateNumber(&PrimaryCCD.ImagePixelSizeNP, values, names, n) == 0)
            {
                PrimaryCCD.ImagePixelSizeNP.s = IPS_OK;
                SetCCDParams(PrimaryCCD.ImagePixelSizeNP.np[CCDChip::CCD_MAX_X].value,
                             PrimaryCCD.ImagePixelSizeNP.np[CCDChip::CCD_MAX_Y].value,
                             PrimaryCCD.getBPP(),
                             PrimaryCCD.ImagePixelSizeNP.np[CCDChip::CCD_PIXEL_SIZE_X].value,
                             PrimaryCCD.ImagePixelSizeNP.np[CCDChip::CCD_PIXEL_SIZE_Y].value);
                saveConfig(true, PrimaryCCD.ImagePixelSizeNP.name);
            }
            else
                PrimaryCCD.ImagePixelSizeNP.s = IPS_ALERT;

            IDSetNumber(&PrimaryCCD.ImagePixelSizeNP, nullptr);
            return true;
        }

        // Guide CCD Info
        if (!strcmp(name, GuideCCD.ImagePixelSizeNP.name))
        {
            IUUpdateNumber(&GuideCCD.ImagePixelSizeNP, values, names, n);
            GuideCCD.ImagePixelSizeNP.s = IPS_OK;
            SetGuiderParams(GuideCCD.ImagePixelSizeNP.np[CCDChip::CCD_MAX_X].value,
                            GuideCCD.ImagePixelSizeNP.np[CCDChip::CCD_MAX_Y].value, GuideCCD.getBPP(),
                            GuideCCD.ImagePixelSizeNP.np[CCDChip::CCD_PIXEL_SIZE_X].value,
                            GuideCCD.ImagePixelSizeNP.np[CCDChip::CCD_PIXEL_SIZE_Y].value);
            IDSetNumber(&GuideCCD.ImagePixelSizeNP, nullptr);
            saveConfig(true);
            return true;
        }

        // CCD Rotation
        if (!strcmp(name, CCDRotationNP.name))
        {
            IUUpdateNumber(&CCDRotationNP, values, names, n);
            CCDRotationNP.s = IPS_OK;
            IDSetNumber(&CCDRotationNP, nullptr);
            m_ValidCCDRotation = true;

            DEBUGF(Logger::DBG_SESSION, "CCD FOV rotation updated to %g degrees.", CCDRotationN[0].value);

            return true;
        }
    }

    // Streamer
    if (HasStreaming())
        Streamer->ISNewNumber(dev, name, values, names, n);

    // DSP
    if (HasDSP())
        DSP->ISNewNumber(dev, name, values, names, n);

    return DefaultDevice::ISNewNumber(dev, name, values, names, n);
}

bool CCD::ISNewSwitch(const char * dev, const char * name, ISState * states, char * names[], int n)
{
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        // Upload Mode
        if (!strcmp(name, UploadSP.name))
        {
            int prevMode = IUFindOnSwitchIndex(&UploadSP);
            IUUpdateSwitch(&UploadSP, states, names, n);

            if (UpdateCCDUploadMode(static_cast<CCD_UPLOAD_MODE>(IUFindOnSwitchIndex(&UploadSP))))
            {
                if (UploadS[UPLOAD_CLIENT].s == ISS_ON)
                {
                    DEBUG(Logger::DBG_SESSION, "Upload settings set to client only.");
                    if (prevMode != 0)
                        deleteProperty(FileNameTP.name);
                }
                else if (UploadS[UPLOAD_LOCAL].s == ISS_ON)
                {
                    DEBUG(Logger::DBG_SESSION, "Upload settings set to local only.");
                    defineProperty(&FileNameTP);
                }
                else
                {
                    DEBUG(Logger::DBG_SESSION, "Upload settings set to client and local.");
                    defineProperty(&FileNameTP);
                }

                UploadSP.s = IPS_OK;
            }
            else
            {
                IUResetSwitch(&UploadSP);
                UploadS[prevMode].s = ISS_ON;
                UploadSP.s = IPS_ALERT;
            }

            IDSetSwitch(&UploadSP, nullptr);

            return true;
        }

        if (!strcmp(name, TelescopeTypeSP.name))
        {
            IUUpdateSwitch(&TelescopeTypeSP, states, names, n);
            TelescopeTypeSP.s = IPS_OK;
            IDSetSwitch(&TelescopeTypeSP, nullptr);
            return true;
        }

        // Fast Exposure Toggle
        if (!strcmp(name, FastExposureToggleSP.name))
        {
            IUUpdateSwitch(&FastExposureToggleSP, states, names, n);

            // Only display warning for the first time this is enabled.
            if (FastExposureToggleSP.s == IPS_IDLE && FastExposureToggleS[INDI_ENABLED].s == ISS_ON)
                LOG_WARN("Experimental Feature: After a frame is downloaded, the next frame capture immediately starts to avoid any delays.");

            if (FastExposureToggleS[INDI_DISABLED].s == ISS_ON)
            {
                FastExposureCountNP.s = IPS_IDLE;
                IDSetNumber(&FastExposureCountNP, nullptr);
                m_UploadTime = 0;
                if (PrimaryCCD.isExposing())
                    AbortExposure();
            }

            FastExposureToggleSP.s = IPS_OK;
            IDSetSwitch(&FastExposureToggleSP, nullptr);
            return true;
        }


#ifdef HAVE_WEBSOCKET
        // Websocket Enable/Disable
        if (!strcmp(name, WebSocketSP.name))
        {
            IUUpdateSwitch(&WebSocketSP, states, names, n);
            WebSocketSP.s = IPS_OK;

            if (WebSocketS[WEBSOCKET_ENABLED].s == ISS_ON)
            {
                wsThread = std::thread(&wsThreadHelper, this);
                WebSocketSettingsN[WS_SETTINGS_PORT].value = wsServer.generatePort();
                WebSocketSettingsNP.s = IPS_OK;
                defineProperty(&WebSocketSettingsNP);
            }
            else if (wsServer.is_running())
            {
                wsServer.stop();
                wsThread.join();
                deleteProperty(WebSocketSettingsNP.name);
            }

            IDSetSwitch(&WebSocketSP, nullptr);
            return true;
        }
#endif

        // WCS Enable/Disable
        if (!strcmp(name, WorldCoordSP.name))
        {
            IUUpdateSwitch(&WorldCoordSP, states, names, n);
            WorldCoordSP.s = IPS_OK;

            if (WorldCoordS[0].s == ISS_ON)
            {
                LOG_INFO("World Coordinate System is enabled.");
                defineProperty(&CCDRotationNP);
            }
            else
            {
                LOG_INFO("World Coordinate System is disabled.");
                deleteProperty(CCDRotationNP.name);
            }

            m_ValidCCDRotation = false;
            IDSetSwitch(&WorldCoordSP, nullptr);
        }

        // Primary Chip Frame Reset
        if (strcmp(name, PrimaryCCD.ResetSP.name) == 0)
        {
            IUResetSwitch(&PrimaryCCD.ResetSP);
            PrimaryCCD.ResetSP.s = IPS_OK;
            if (CanBin())
                UpdateCCDBin(1, 1);
            if (CanSubFrame())
                UpdateCCDFrame(0, 0, PrimaryCCD.getXRes(), PrimaryCCD.getYRes());

            IDSetSwitch(&PrimaryCCD.ResetSP, nullptr);
            return true;
        }

        // Primary Chip Abort Expsoure
        if (strcmp(name, PrimaryCCD.AbortExposureSP.name) == 0)
        {
            IUResetSwitch(&PrimaryCCD.AbortExposureSP);

            if (AbortExposure())
            {
                PrimaryCCD.AbortExposureSP.s       = IPS_OK;
                PrimaryCCD.ImageExposureNP.s       = IPS_IDLE;
                PrimaryCCD.ImageExposureN[0].value = 0;
            }
            else
            {
                PrimaryCCD.AbortExposureSP.s = IPS_ALERT;
                PrimaryCCD.ImageExposureNP.s = IPS_ALERT;
            }

            setCurrentPollingPeriod(getPollingPeriod());

            // Fast Exposure Count
            if (FastExposureCountNP.s == IPS_BUSY)
            {
                m_UploadTime = 0;
                FastExposureCountNP.s = IPS_IDLE;
                FastExposureCountN[0].value = 1;
                IDSetNumber(&FastExposureCountNP, nullptr);
            }

            IDSetSwitch(&PrimaryCCD.AbortExposureSP, nullptr);
            IDSetNumber(&PrimaryCCD.ImageExposureNP, nullptr);

            return true;
        }

        // Guide Chip Abort Exposure
        if (strcmp(name, GuideCCD.AbortExposureSP.name) == 0)
        {
            IUResetSwitch(&GuideCCD.AbortExposureSP);

            if (AbortGuideExposure())
            {
                GuideCCD.AbortExposureSP.s       = IPS_OK;
                GuideCCD.ImageExposureNP.s       = IPS_IDLE;
                GuideCCD.ImageExposureN[0].value = 0;
            }
            else
            {
                GuideCCD.AbortExposureSP.s = IPS_ALERT;
                GuideCCD.ImageExposureNP.s = IPS_ALERT;
            }

            IDSetSwitch(&GuideCCD.AbortExposureSP, nullptr);
            IDSetNumber(&GuideCCD.ImageExposureNP, nullptr);

            return true;
        }

        // Primary Chip Compression
        if (strcmp(name, PrimaryCCD.CompressSP.name) == 0)
        {
            IUUpdateSwitch(&PrimaryCCD.CompressSP, states, names, n);
            PrimaryCCD.CompressSP.s = IPS_OK;
            IDSetSwitch(&PrimaryCCD.CompressSP, nullptr);
            PrimaryCCD.SendCompressed = PrimaryCCD.CompressS[INDI_ENABLED].s == ISS_ON;
            return true;
        }

        // Guide Chip Compression
        if (strcmp(name, GuideCCD.CompressSP.name) == 0)
        {
            IUUpdateSwitch(&GuideCCD.CompressSP, states, names, n);
            GuideCCD.CompressSP.s = IPS_OK;
            IDSetSwitch(&GuideCCD.CompressSP, nullptr);
            GuideCCD.SendCompressed = GuideCCD.CompressS[INDI_ENABLED].s == ISS_ON;
            return true;
        }

        // Primary Chip Frame Type
        if (strcmp(name, PrimaryCCD.FrameTypeSP.name) == 0)
        {
            IUUpdateSwitch(&PrimaryCCD.FrameTypeSP, states, names, n);
            PrimaryCCD.FrameTypeSP.s = IPS_OK;
            if (PrimaryCCD.FrameTypeS[0].s == ISS_ON)
                PrimaryCCD.setFrameType(CCDChip::LIGHT_FRAME);
            else if (PrimaryCCD.FrameTypeS[1].s == ISS_ON)
            {
                PrimaryCCD.setFrameType(CCDChip::BIAS_FRAME);
                if (HasShutter() == false)
                    DEBUG(Logger::DBG_WARNING,
                          "The CCD does not have a shutter. Cover the camera in order to take a bias frame.");
            }
            else if (PrimaryCCD.FrameTypeS[2].s == ISS_ON)
            {
                PrimaryCCD.setFrameType(CCDChip::DARK_FRAME);
                if (HasShutter() == false)
                    DEBUG(Logger::DBG_WARNING,
                          "The CCD does not have a shutter. Cover the camera in order to take a dark frame.");
            }
            else if (PrimaryCCD.FrameTypeS[3].s == ISS_ON)
                PrimaryCCD.setFrameType(CCDChip::FLAT_FRAME);

            if (UpdateCCDFrameType(PrimaryCCD.getFrameType()) == false)
                PrimaryCCD.FrameTypeSP.s = IPS_ALERT;

            IDSetSwitch(&PrimaryCCD.FrameTypeSP, nullptr);

            return true;
        }

        // Guide Chip Frame Type
        if (strcmp(name, GuideCCD.FrameTypeSP.name) == 0)
        {
            //  Compression Update
            IUUpdateSwitch(&GuideCCD.FrameTypeSP, states, names, n);
            GuideCCD.FrameTypeSP.s = IPS_OK;
            if (GuideCCD.FrameTypeS[0].s == ISS_ON)
                GuideCCD.setFrameType(CCDChip::LIGHT_FRAME);
            else if (GuideCCD.FrameTypeS[1].s == ISS_ON)
            {
                GuideCCD.setFrameType(CCDChip::BIAS_FRAME);
                if (HasShutter() == false)
                    DEBUG(Logger::DBG_WARNING,
                          "The CCD does not have a shutter. Cover the camera in order to take a bias frame.");
            }
            else if (GuideCCD.FrameTypeS[2].s == ISS_ON)
            {
                GuideCCD.setFrameType(CCDChip::DARK_FRAME);
                if (HasShutter() == false)
                    DEBUG(Logger::DBG_WARNING,
                          "The CCD does not have a shutter. Cover the camera in order to take a dark frame.");
            }
            else if (GuideCCD.FrameTypeS[3].s == ISS_ON)
                GuideCCD.setFrameType(CCDChip::FLAT_FRAME);

            if (UpdateGuiderFrameType(GuideCCD.getFrameType()) == false)
                GuideCCD.FrameTypeSP.s = IPS_ALERT;

            IDSetSwitch(&GuideCCD.FrameTypeSP, nullptr);

            return true;
        }

        // Capture Format
        if (CaptureFormatSP.isNameMatch(name))
        {
            int previousIndex = CaptureFormatSP.findOnSwitchIndex();
            CaptureFormatSP.update(states, names, n);

            if (SetCaptureFormat(CaptureFormatSP.findOnSwitchIndex()))
                CaptureFormatSP.setState(IPS_OK);
            else
            {
                if (previousIndex >= 0)
                {
                    CaptureFormatSP.reset();
                    CaptureFormatSP[previousIndex].setState(ISS_ON);
                }
                CaptureFormatSP.setState(IPS_ALERT);
            }
            CaptureFormatSP.apply();

            if (m_ConfigCaptureFormatLabel != CaptureFormatSP.findOnSwitch()->getLabel())
            {
                m_ConfigCaptureFormatLabel = CaptureFormatSP.findOnSwitch()->getLabel();
                saveConfig(true, CaptureFormatSP.getName());
            }

            return true;
        }

        // Encode Format
        if (EncodeFormatSP.isNameMatch(name))
        {
            EncodeFormatSP.update(states, names, n);
            EncodeFormatSP.setState(IPS_OK);
            EncodeFormatSP.apply();

            if (m_ConfigEncodeFormatIndex != EncodeFormatSP.findOnSwitchIndex())
            {
                m_ConfigEncodeFormatIndex = EncodeFormatSP.findOnSwitchIndex();
                saveConfig(true, EncodeFormatSP.getName());
            }

            return true;
        }

#if 0
        // Primary Chip Rapid Guide Enable/Disable
        if (strcmp(name, PrimaryCCD.RapidGuideSP.name) == 0)
        {
            IUUpdateSwitch(&PrimaryCCD.RapidGuideSP, states, names, n);
            PrimaryCCD.RapidGuideSP.s = IPS_OK;
            RapidGuideEnabled         = (PrimaryCCD.RapidGuideS[0].s == ISS_ON);

            if (RapidGuideEnabled)
            {
                defineProperty(&PrimaryCCD.RapidGuideSetupSP);
                defineProperty(&PrimaryCCD.RapidGuideDataNP);
            }
            else
            {
                deleteProperty(PrimaryCCD.RapidGuideSetupSP.name);
                deleteProperty(PrimaryCCD.RapidGuideDataNP.name);
            }

            IDSetSwitch(&PrimaryCCD.RapidGuideSP, nullptr);
            return true;
        }

        // Guide Chip Rapid Guide Enable/Disable
        if (strcmp(name, GuideCCD.RapidGuideSP.name) == 0)
        {
            IUUpdateSwitch(&GuideCCD.RapidGuideSP, states, names, n);
            GuideCCD.RapidGuideSP.s = IPS_OK;
            GuiderRapidGuideEnabled = (GuideCCD.RapidGuideS[0].s == ISS_ON);

            if (GuiderRapidGuideEnabled)
            {
                defineProperty(&GuideCCD.RapidGuideSetupSP);
                defineProperty(&GuideCCD.RapidGuideDataNP);
            }
            else
            {
                deleteProperty(GuideCCD.RapidGuideSetupSP.name);
                deleteProperty(GuideCCD.RapidGuideDataNP.name);
            }

            IDSetSwitch(&GuideCCD.RapidGuideSP, nullptr);
            return true;
        }

        // Primary CCD Rapid Guide Setup
        if (strcmp(name, PrimaryCCD.RapidGuideSetupSP.name) == 0)
        {
            IUUpdateSwitch(&PrimaryCCD.RapidGuideSetupSP, states, names, n);
            PrimaryCCD.RapidGuideSetupSP.s = IPS_OK;

            AutoLoop   = (PrimaryCCD.RapidGuideSetupS[0].s == ISS_ON);
            SendImage  = (PrimaryCCD.RapidGuideSetupS[1].s == ISS_ON);
            ShowMarker = (PrimaryCCD.RapidGuideSetupS[2].s == ISS_ON);

            IDSetSwitch(&PrimaryCCD.RapidGuideSetupSP, nullptr);
            return true;
        }

        // Guide Chip Rapid Guide Setup
        if (strcmp(name, GuideCCD.RapidGuideSetupSP.name) == 0)
        {
            IUUpdateSwitch(&GuideCCD.RapidGuideSetupSP, states, names, n);
            GuideCCD.RapidGuideSetupSP.s = IPS_OK;

            GuiderAutoLoop   = (GuideCCD.RapidGuideSetupS[0].s == ISS_ON);
            GuiderSendImage  = (GuideCCD.RapidGuideSetupS[1].s == ISS_ON);
            GuiderShowMarker = (GuideCCD.RapidGuideSetupS[2].s == ISS_ON);

            IDSetSwitch(&GuideCCD.RapidGuideSetupSP, nullptr);
            return true;
        }
#endif
    }

    if (HasStreaming())
        Streamer->ISNewSwitch(dev, name, states, names, n);

    // DSP
    if (HasDSP())
        DSP->ISNewSwitch(dev, name, states, names, n);

    return DefaultDevice::ISNewSwitch(dev, name, states, names, n);
}

bool CCD::ISNewBLOB(const char *dev, const char *name, int sizes[], int blobsizes[], char *blobs[],
                    char *formats[], char *names[], int n)
{
    // DSP
    if (HasDSP())
        DSP->ISNewBLOB(dev, name, sizes, blobsizes, blobs, formats, names, n);

    return DefaultDevice::ISNewBLOB(dev, name, sizes, blobsizes, blobs, formats, names, n);
}

int CCD::SetTemperature(double temperature)
{
    INDI_UNUSED(temperature);
    DEBUGF(Logger::DBG_WARNING, "CCD::SetTemperature %4.2f -  Should never get here", temperature);
    return -1;
}

bool CCD::StartExposure(float duration)
{
    DEBUGF(Logger::DBG_WARNING, "CCD::StartExposure %4.2f -  Should never get here", duration);
    return false;
}

bool CCD::StartGuideExposure(float duration)
{
    DEBUGF(Logger::DBG_WARNING, "CCD::StartGuide Exposure %4.2f -  Should never get here", duration);
    return false;
}

bool CCD::AbortExposure()
{
    DEBUG(Logger::DBG_WARNING, "CCD::AbortExposure -  Should never get here");
    return false;
}

bool CCD::AbortGuideExposure()
{
    DEBUG(Logger::DBG_WARNING, "CCD::AbortGuideExposure -  Should never get here");
    return false;
}

bool CCD::UpdateCCDFrame(int x, int y, int w, int h)
{
    // Just set value, unless HW layer overrides this and performs its own processing
    PrimaryCCD.setFrame(x, y, w, h);
    return true;
}

bool CCD::UpdateGuiderFrame(int x, int y, int w, int h)
{
    GuideCCD.setFrame(x, y, w, h);
    return true;
}

bool CCD::UpdateCCDBin(int hor, int ver)
{
    // Just set value, unless HW layer overrides this and performs its own processing
    PrimaryCCD.setBin(hor, ver);
    // Reset size
    if (HasStreaming())
        Streamer->setSize(PrimaryCCD.getSubW() / hor, PrimaryCCD.getSubH() / ver);

    // DSP
    if (HasDSP())
        DSP->setSizes(2, new int[2] { PrimaryCCD.getSubW() / hor, PrimaryCCD.getSubH() / ver });

    return true;
}

bool CCD::UpdateGuiderBin(int hor, int ver)
{
    // Just set value, unless HW layer overrides this and performs its own processing
    GuideCCD.setBin(hor, ver);
    return true;
}

bool CCD::UpdateCCDFrameType(CCDChip::CCD_FRAME fType)
{
    INDI_UNUSED(fType);
    // Child classes can override this
    return true;
}

bool CCD::UpdateGuiderFrameType(CCDChip::CCD_FRAME fType)
{
    INDI_UNUSED(fType);
    // Child classes can override this
    return true;
}

void CCD::addFITSKeywords(CCDChip * targetChip)
{
    int status = 0;
    char dev_name[MAXINDINAME] = {0};
    double effectiveFocalLength = std::numeric_limits<double>::quiet_NaN();
    double effectiveAperture = std::numeric_limits<double>::quiet_NaN();

    auto fptr = *targetChip->fitsFilePointer();

    AutoCNumeric locale;
    fits_update_key_str(fptr, "ROWORDER", "TOP-DOWN", "Row Order", &status);
    fits_update_key_str(fptr, "INSTRUME", getDeviceName(), "CCD Name", &status);

    // Telescope
    if (strlen(ActiveDeviceT[ACTIVE_TELESCOPE].text) > 0)
    {
        fits_update_key_str(fptr, "TELESCOP", ActiveDeviceT[0].text, "Telescope name", &status);
    }

    // Which scope is in effect
    // TODO: Support N-telescopes
    if (TelescopeTypeS[TELESCOPE_PRIMARY].s == ISS_ON)
    {
        if (primaryFocalLength > 0)
            effectiveFocalLength = primaryFocalLength;
        if (primaryAperture > 0)
            effectiveAperture = primaryAperture;
    }
    else if (TelescopeTypeS[TELESCOPE_GUIDE].s == ISS_ON)
    {
        if (guiderFocalLength > 0)
            effectiveFocalLength = guiderFocalLength;
        if (guiderAperture > 0)
            effectiveAperture = guiderAperture;
    }

    if (std::isnan(effectiveFocalLength))
        LOG_WARN("Telescope focal length is missing.");
    if (std::isnan(effectiveAperture))
        LOG_WARN("Telescope aperture is missing.");

    // Observer
    fits_update_key_str(fptr, "OBSERVER", FITSHeaderT[FITS_OBSERVER].text, "Observer name", &status);

    // Object
    fits_update_key_str(fptr, "OBJECT", FITSHeaderT[FITS_OBJECT].text, "Object name", &status);

    double subPixSize1 = static_cast<double>(targetChip->getPixelSizeX());
    double subPixSize2 = static_cast<double>(targetChip->getPixelSizeY());
    uint32_t subW = targetChip->getSubW();
    uint32_t subH = targetChip->getSubH();
    uint32_t subBinX = targetChip->getBinX();
    uint32_t subBinY = targetChip->getBinY();

    strncpy(dev_name, getDeviceName(), MAXINDINAME);

    fits_update_key_dbl(fptr, "EXPTIME", exposureDuration, 6, "Total Exposure Time (s)", &status);

    if (targetChip->getFrameType() == CCDChip::DARK_FRAME)
        fits_update_key_dbl(fptr, "DARKTIME", exposureDuration, 6, "Total Dark Exposure Time (s)", &status);

    // If the camera has a cooler OR if the temperature permission was explicitly set to Read-Only, then record the temperature
    if (HasCooler() || TemperatureNP.p == IP_RO)
        fits_update_key_dbl(fptr, "CCD-TEMP", TemperatureN[0].value, 3, "CCD Temperature (Celsius)", &status);

    fits_update_key_dbl(fptr, "PIXSIZE1", subPixSize1, 6, "Pixel Size 1 (microns)", &status);
    fits_update_key_dbl(fptr, "PIXSIZE2", subPixSize2, 6, "Pixel Size 2 (microns)", &status);
    fits_update_key_lng(fptr, "XBINNING", targetChip->getBinX(), "Binning factor in width", &status);
    fits_update_key_lng(fptr, "YBINNING", targetChip->getBinY(), "Binning factor in height", &status);
    // XPIXSZ and YPIXSZ are logical sizes including the binning factor
    double xpixsz = subPixSize1 * subBinX;
    double ypixsz = subPixSize2 * subBinY;
    fits_update_key_dbl(fptr, "XPIXSZ", xpixsz, 6, "X binned pixel size in microns", &status);
    fits_update_key_dbl(fptr, "YPIXSZ", ypixsz, 6, "Y binned pixel size in microns", &status);

    switch (targetChip->getFrameType())
    {
        case CCDChip::LIGHT_FRAME:
            fits_update_key_str(fptr, "FRAME", "Light", "Frame Type", &status);
            fits_update_key_str(fptr, "IMAGETYP", "Light Frame", "Frame Type", &status);
            break;
        case CCDChip::BIAS_FRAME:
            fits_update_key_str(fptr, "FRAME", "Bias", "Frame Type", &status);
            fits_update_key_str(fptr, "IMAGETYP", "Bias Frame", "Frame Type", &status);
            break;
        case CCDChip::FLAT_FRAME:
            fits_update_key_str(fptr, "FRAME", "Flat", "Frame Type", &status);
            fits_update_key_str(fptr, "IMAGETYP", "Flat Frame", "Frame Type", &status);
            break;
        case CCDChip::DARK_FRAME:
            fits_update_key_str(fptr, "FRAME", "Dark", "Frame Type", &status);
            fits_update_key_str(fptr, "IMAGETYP", "Dark Frame", "Frame Type", &status);
            break;
    }

    if (CurrentFilterSlot != -1 && CurrentFilterSlot <= static_cast<int>(FilterNames.size()))
    {
        fits_update_key_str(fptr, "FILTER", FilterNames.at(CurrentFilterSlot - 1).c_str(), "Filter", &status);
    }

#ifdef WITH_MINMAX
    if (targetChip->getNAxis() == 2)
    {
        double min_val, max_val;
        getMinMax(&min_val, &max_val, targetChip);

        fits_update_key_dbl(fptr, "DATAMIN", min_val, 6, "Minimum value", &status);
        fits_update_key_dbl(fptr, "DATAMAX", max_val, 6, "Maximum value", &status);
    }
#endif

    if (HasBayer() && targetChip->getNAxis() == 2)
    {
        fits_update_key_lng(fptr, "XBAYROFF", atoi(BayerT[0].text), "X offset of Bayer array", &status);
        fits_update_key_lng(fptr, "YBAYROFF", atoi(BayerT[1].text), "Y offset of Bayer array", &status);
        fits_update_key_str(fptr, "BAYERPAT", BayerT[2].text, "Bayer color pattern", &status);
    }

    if (!std::isnan(effectiveFocalLength))
        fits_update_key_dbl(fptr, "FOCALLEN", effectiveFocalLength, 3, "Focal Length (mm)", &status);

    if (!std::isnan(effectiveAperture))
        fits_update_key_dbl(fptr, "APTDIA", effectiveAperture, 3, "Telescope diameter (mm)", &status);

    if (!std::isnan(MPSAS))
    {
        fits_update_key_dbl(fptr, "MPSAS", MPSAS, 6, "Sky Quality (mag per arcsec^2)", &status);
    }

    if (!std::isnan(RotatorAngle))
    {
        fits_update_key_dbl(fptr, "ROTATANG", RotatorAngle, 3, "Rotator angle in degrees", &status);
    }

    // JJ ed 2020-03-28
    // If the focus position or temperature is set, add the information to the FITS header
    if (FocuserPos != -1)
    {
        fits_update_key_lng(fptr, "FOCUSPOS", FocuserPos, "Focus position in steps", &status);
    }
    if (!std::isnan(FocuserTemp))
    {
        fits_update_key_dbl(fptr, "FOCUSTEM", FocuserTemp, 3, "Focuser temperature in degrees C", &status);
    }

    // SCALE assuming square-pixels
    if (!std::isnan(effectiveFocalLength))
    {
        double pixScale = subPixSize1 / effectiveFocalLength * 206.3 * subBinX;
        fits_update_key_dbl(fptr, "SCALE", pixScale, 6, "arcsecs per pixel", &status);
    }


    if ( targetChip->getFrameType() == CCDChip::LIGHT_FRAME && !std::isnan(RA) && !std::isnan(Dec) && (std::isnan(J2000RA)
            || std::isnan(J2000DE) || !J2000Valid) )
    {
        INDI::IEquatorialCoordinates epochPos { 0, 0 }, J2000Pos { 0, 0 };
        epochPos.rightascension  = RA;
        epochPos.declination = Dec;

        // Convert from JNow to J2000
        INDI::ObservedToJ2000(&epochPos, ln_get_julian_from_sys(), &J2000Pos);

        J2000RA = J2000Pos.rightascension;
        J2000DE = J2000Pos.declination;
    }
    J2000Valid = false;  // enforce usage of EOD position if we receive no new epoch position

    if ( targetChip->getFrameType() == CCDChip::LIGHT_FRAME && !std::isnan(J2000RA) && !std::isnan(J2000DE) )
    {
        if (!std::isnan(Latitude) && !std::isnan(Longitude))
        {
            INDI::IEquatorialCoordinates epochPos { 0, 0 }, J2000Pos { 0, 0 };

            J2000Pos.rightascension = J2000RA;
            J2000Pos.declination = J2000DE;

            // Convert from JNow to J2000
            INDI::J2000toObserved(&J2000Pos, ln_get_julian_from_sys(), &epochPos);

            // Horizontal Coords
            INDI::IHorizontalCoordinates horizontalPos;
            IGeographicCoordinates observer;
            observer.latitude = Latitude;
            observer.longitude = Longitude;

            EquatorialToHorizontal(&epochPos, &observer, ln_get_julian_from_sys(), &horizontalPos);
            Azimuth = horizontalPos.azimuth;
            Altitude = horizontalPos.altitude;
            Airmass = ln_get_airmass(Altitude, 750);
        }

        char ra_str[32] = {0}, de_str[32] = {0};

        fs_sexa(ra_str, J2000RA, 2, 360000);
        fs_sexa(de_str, J2000DE, 2, 360000);

        char * raPtr = ra_str, *dePtr = de_str;
        while (*raPtr != '\0')
        {
            if (*raPtr == ':')
                *raPtr = ' ';
            raPtr++;
        }
        while (*dePtr != '\0')
        {
            if (*dePtr == ':')
                *dePtr = ' ';
            dePtr++;
        }

        if (!std::isnan(Latitude) && !std::isnan(Longitude))
        {
            fits_update_key_dbl(fptr, "SITELAT", Latitude, 6, "Latitude of the imaging site in degrees", &status);
            fits_update_key_dbl(fptr, "SITELONG", Longitude, 6, "Longitude of the imaging site in degrees", &status);
        }
        if (!std::isnan(Airmass))
        {
            //fits_update_key_s(fptr, TDOUBLE, "AIRMASS", &Airmass, "Airmass", &status);
            fits_update_key_dbl(fptr, "AIRMASS", Airmass, 6, "Airmass", &status);
            fits_update_key_dbl(fptr, "OBJCTAZ", Azimuth, 6, "Azimuth of center of image in Degrees", &status);
            fits_update_key_dbl(fptr, "OBJCTALT", Altitude, 6, "Altitude of center of image in Degrees", &status);
        }
        fits_update_key_str(fptr, "OBJCTRA", ra_str, "Object J2000 RA in Hours", &status);
        fits_update_key_str(fptr, "OBJCTDEC", de_str, "Object J2000 DEC in Degrees", &status);

        fits_update_key_dbl(fptr, "RA", J2000RA * 15, 6, "Object J2000 RA in Degrees", &status);
        fits_update_key_dbl(fptr, "DEC", J2000DE, 6, "Object J2000 DEC in Degrees", &status);

        // pier side
        switch (pierSide)
        {
            case 0:
                fits_update_key_str(fptr, "PIERSIDE", "WEST", "West, looking East", &status);
                break;
            case 1:
                fits_update_key_str(fptr, "PIERSIDE", "EAST", "East, looking West", &status);
                break;
        }

        //fits_update_key_s(fptr, TINT, "EPOCH", &epoch, "Epoch", &status);
        fits_update_key_lng(fptr, "EQUINOX", 2000, "Equinox", &status);

        // Add WCS Info
        if (WorldCoordS[0].s == ISS_ON && m_ValidCCDRotation && !std::isnan(effectiveFocalLength))
        {
            double J2000RAHours = J2000RA * 15;
            fits_update_key_dbl(fptr, "CRVAL1", J2000RAHours, 10, "CRVAL1", &status);
            fits_update_key_dbl(fptr, "CRVAL2", J2000DE, 10, "CRVAL1", &status);

            char radecsys[8] = "FK5";
            char ctype1[16]  = "RA---TAN";
            char ctype2[16]  = "DEC--TAN";

            fits_update_key_str(fptr, "RADECSYS", radecsys, "RADECSYS", &status);
            fits_update_key_str(fptr, "CTYPE1", ctype1, "CTYPE1", &status);
            fits_update_key_str(fptr, "CTYPE2", ctype2, "CTYPE2", &status);

            double crpix1 = subW / subBinX / 2.0;
            double crpix2 = subH / subBinY / 2.0;

            fits_update_key_dbl(fptr, "CRPIX1", crpix1, 10, "CRPIX1", &status);
            fits_update_key_dbl(fptr, "CRPIX2", crpix2, 10, "CRPIX2", &status);

            double secpix1 = subPixSize1 / effectiveFocalLength * 206.3 * subBinX;
            double secpix2 = subPixSize2 / effectiveFocalLength * 206.3 * subBinY;

            fits_update_key_dbl(fptr, "SECPIX1", secpix1, 10, "SECPIX1", &status);
            fits_update_key_dbl(fptr, "SECPIX2", secpix2, 10, "SECPIX2", &status);

            double degpix1 = secpix1 / 3600.0;
            double degpix2 = secpix2 / 3600.0;

            fits_update_key_dbl(fptr, "CDELT1", degpix1, 10, "CDELT1", &status);
            fits_update_key_dbl(fptr, "CDELT2", degpix2, 10, "CDELT2", &status);

            // Rotation is CW, we need to convert it to CCW per CROTA1 definition
            double rotation = 360 - CCDRotationN[0].value;
            if (rotation > 360)
                rotation -= 360;

            fits_update_key_dbl(fptr, "CROTA1", rotation, 10, "CROTA1", &status);
            fits_update_key_dbl(fptr, "CROTA2", rotation, 10, "CROTA2", &status);

            /*double cd[4];
            cd[0] = degpix1;
            cd[1] = 0;
            cd[2] = 0;
            cd[3] = degpix2;

            fits_update_key_s(fptr, TDOUBLE, "CD1_1", &cd[0], "CD1_1", &status);
            fits_update_key_s(fptr, TDOUBLE, "CD1_2", &cd[1], "CD1_2", &status);
            fits_update_key_s(fptr, TDOUBLE, "CD2_1", &cd[2], "CD2_1", &status);
            fits_update_key_s(fptr, TDOUBLE, "CD2_2", &cd[3], "CD2_2", &status);*/
        }
    }

    fits_update_key_str(fptr, "DATE-OBS", exposureStartTime, "UTC start date of observation", &status);
    fits_write_comment(fptr, "Generated by INDI", &status);
}

void CCD::fits_update_key_s(fitsfile * fptr, int type, std::string name, void * p, std::string explanation,
                            int * status)
{
    // this function is for removing warnings about deprecated string conversion to char* (from arg 5)
    fits_update_key(fptr, type, name.c_str(), p, const_cast<char *>(explanation.c_str()), status);
}

bool CCD::ExposureComplete(CCDChip * targetChip)
{
    // Reset POLLMS to default value
    setCurrentPollingPeriod(getPollingPeriod());

    // Run async
    std::thread(&CCD::ExposureCompletePrivate, this, targetChip).detach();

    return true;
}

bool CCD::ExposureCompletePrivate(CCDChip * targetChip)
{
    // save information used for the fits header
    exposureDuration = targetChip->getExposureDuration();
    strncpy(exposureStartTime, targetChip->getExposureStartTime(), MAXINDINAME);

    if(HasDSP())
    {
        uint8_t* buf = static_cast<uint8_t*>(malloc(targetChip->getFrameBufferSize()));
        memcpy(buf, targetChip->getFrameBuffer(), targetChip->getFrameBufferSize());
        DSP->processBLOB(buf, 2, new int[2] { targetChip->getXRes() / targetChip->getBinX(), targetChip->getYRes() / targetChip->getBinY() },
                         targetChip->getBPP());
        free(buf);
    }

    if (processFastExposure(targetChip) == false)
        return false;

    bool sendImage = (UploadS[UPLOAD_CLIENT].s == ISS_ON || UploadS[UPLOAD_BOTH].s == ISS_ON);
    bool saveImage = (UploadS[UPLOAD_LOCAL].s == ISS_ON || UploadS[UPLOAD_BOTH].s == ISS_ON);

    // Do not send or save an empty image.
    if (targetChip->getFrameBufferSize() == 0)
        sendImage = saveImage = false;

    if (sendImage || saveImage)
    {
        if (EncodeFormatSP[FORMAT_FITS].getState() == ISS_ON)
        {
            int img_type  = 0;
            int byte_type = 0;
            int status    = 0;
            long naxis    = targetChip->getNAxis();
            long naxes[3];
            int nelements = 0;
            std::string bit_depth;
            char error_status[MAXRBUF];

            naxes[0] = targetChip->getSubW() / targetChip->getBinX();
            naxes[1] = targetChip->getSubH() / targetChip->getBinY();

            switch (targetChip->getBPP())
            {
                case 8:
                    byte_type = TBYTE;
                    img_type  = BYTE_IMG;
                    bit_depth = "8 bits per pixel";
                    break;

                case 16:
                    byte_type = TUSHORT;
                    img_type  = USHORT_IMG;
                    bit_depth = "16 bits per pixel";
                    break;

                case 32:
                    byte_type = TULONG;
                    img_type  = ULONG_IMG;
                    bit_depth = "32 bits per pixel";
                    break;

                default:
                    LOGF_ERROR("Unsupported bits per pixel value %d", targetChip->getBPP());
                    return false;
            }

            nelements = naxes[0] * naxes[1];
            if (naxis == 3)
            {
                nelements *= 3;
                naxes[2] = 3;
            }

            /*DEBUGF(Logger::DBG_DEBUG, "Exposure complete. Image Depth: %s. Width: %d Height: %d nelements: %d", bit_depth.c_str(), naxes[0],
                    naxes[1], nelements);*/

            std::unique_lock<std::mutex> guard(ccdBufferLock);

            // 8640 = 2880 * 3 which is sufficient for most cases.
            uint32_t size = 8640 + nelements * (targetChip->getBPP() / 8);
            //  Initialize FITS file.
            if (targetChip->openFITSFile(size, status) == false)
            {
                fits_report_error(stderr, status); /* print out any error messages */
                fits_get_errstatus(status, error_status);
                LOGF_ERROR("FITS Error: %s", error_status);
                return false;
            }

            auto fptr = *targetChip->fitsFilePointer();

            fits_create_img(fptr, img_type, naxis, naxes, &status);

            if (status)
            {
                fits_report_error(stderr, status); /* print out any error messages */
                fits_get_errstatus(status, error_status);
                LOGF_ERROR("FITS Error: %s", error_status);
                targetChip->closeFITSFile();
                return false;
            }

            addFITSKeywords(targetChip);

            fits_write_img(fptr, byte_type, 1, nelements, targetChip->getFrameBuffer(), &status);
            fits_flush_file(fptr, &status);

            if (status)
            {
                fits_report_error(stderr, status); /* print out any error messages */
                fits_get_errstatus(status, error_status);
                LOGF_ERROR("FITS Error: %s", error_status);
                targetChip->closeFITSFile();
                return false;
            }


            bool rc = uploadFile(targetChip, *(targetChip->fitsMemoryBlockPointer()), *(targetChip->fitsMemorySizePointer()), sendImage,
                                 saveImage);

            targetChip->closeFITSFile();

            guard.unlock();

            if (rc == false)
            {
                targetChip->setExposureFailed();
                return false;
            }
        }
        else
        {
            // If image extension was set to fits (default), change if bin if not already set to another format by the driver.
            if (!strcmp(targetChip->getImageExtension(), "fits"))
                targetChip->setImageExtension("bin");
            std::unique_lock<std::mutex> guard(ccdBufferLock);
            bool rc = uploadFile(targetChip, targetChip->getFrameBuffer(), targetChip->getFrameBufferSize(), sendImage,
                                 saveImage);
            guard.unlock();

            if (rc == false)
            {
                targetChip->setExposureFailed();
                return false;
            }
        }
    }

    if (FastExposureToggleS[INDI_ENABLED].s != ISS_ON)
        targetChip->setExposureComplete();
    return true;
}

bool CCD::uploadFile(CCDChip * targetChip, const void * fitsData, size_t totalBytes, bool sendImage,
                     bool saveImage)
{
    uint8_t * compressedData = nullptr;

    DEBUGF(Logger::DBG_DEBUG, "Uploading file. Ext: %s, Size: %d, sendImage? %s, saveImage? %s",
           targetChip->getImageExtension(), totalBytes, sendImage ? "Yes" : "No", saveImage ? "Yes" : "No");

    if (saveImage)
    {
        targetChip->FitsB.blob    = const_cast<void *>(fitsData);
        targetChip->FitsB.bloblen = totalBytes;
        snprintf(targetChip->FitsB.format, MAXINDIBLOBFMT, ".%s", targetChip->getImageExtension());

        FILE * fp = nullptr;
        char imageFileName[MAXRBUF];

        std::string prefix = UploadSettingsT[UPLOAD_PREFIX].text;
        int maxIndex       = getFileIndex(UploadSettingsT[UPLOAD_DIR].text, UploadSettingsT[UPLOAD_PREFIX].text,
                                          targetChip->FitsB.format);

        if (maxIndex < 0)
        {
            LOGF_ERROR("Error iterating directory %s. %s", UploadSettingsT[0].text,
                       strerror(errno));
            return false;
        }

        if (maxIndex > 0)
        {
            char ts[32];
            struct tm * tp;
            time_t t;
            time(&t);
            tp = localtime(&t);
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H-%M-%S", tp);
            std::string filets(ts);
            prefix = std::regex_replace(prefix, std::regex("ISO8601"), filets);

            char indexString[8];
            snprintf(indexString, 8, "%03d", maxIndex);
            std::string prefixIndex = indexString;
            //prefix.replace(prefix.find("XXX"), std::string::npos, prefixIndex);
            prefix = std::regex_replace(prefix, std::regex("XXX"), prefixIndex);
        }

        snprintf(imageFileName, MAXRBUF, "%s/%s%s", UploadSettingsT[0].text, prefix.c_str(), targetChip->FitsB.format);

        fp = fopen(imageFileName, "w");
        if (fp == nullptr)
        {
            LOGF_ERROR("Unable to save image file (%s). %s", imageFileName, strerror(errno));
            return false;
        }

        int n = 0;
        for (int nr = 0; nr < targetChip->FitsB.bloblen; nr += n)
            n = fwrite((static_cast<char *>(targetChip->FitsB.blob) + nr), 1, targetChip->FitsB.bloblen - nr, fp);

        fclose(fp);

        // Save image file path
        IUSaveText(&FileNameT[0], imageFileName);

        DEBUGF(Logger::DBG_SESSION, "Image saved to %s", imageFileName);
        FileNameTP.s = IPS_OK;
        IDSetText(&FileNameTP, nullptr);
    }

    if (targetChip->SendCompressed)
    {
        if (EncodeFormatSP[FORMAT_FITS].getState() == ISS_ON && !strcmp(targetChip->getImageExtension(), "fits"))
        {
            fpstate	fpvar;
            fp_init (&fpvar);
            size_t compressedBytes = 0;
            int islossless = 0;
            if (fp_pack_data_to_data(reinterpret_cast<const char *>(fitsData), totalBytes, &compressedData, &compressedBytes, fpvar,
                                     &islossless) < 0)
            {
                free(compressedData);
                LOG_ERROR("Error: Ran out of memory compressing image");
                return false;
            }

            targetChip->FitsB.blob    = compressedData;
            targetChip->FitsB.bloblen = compressedBytes;
            snprintf(targetChip->FitsB.format, MAXINDIBLOBFMT, ".%s.fz", targetChip->getImageExtension());
        }
        else
        {
            uLong compressedBytes = sizeof(char) * totalBytes + totalBytes / 64 + 16 + 3;
            compressedData  = new uint8_t[compressedBytes];

            if (fitsData == nullptr || compressedData == nullptr)
            {
                if (compressedData)
                    delete [] compressedData;
                LOG_ERROR("Error: Ran out of memory compressing image");
                return false;
            }

            int r = compress2(compressedData, &compressedBytes, (const Bytef *)fitsData, totalBytes, 9);
            if (r != Z_OK)
            {
                /* this should NEVER happen */
                LOG_ERROR("Error: Failed to compress image");
                delete [] compressedData;
                return false;
            }

            targetChip->FitsB.blob    = compressedData;
            targetChip->FitsB.bloblen = compressedBytes;
            snprintf(targetChip->FitsB.format, MAXINDIBLOBFMT, ".%s.z", targetChip->getImageExtension());
        }
    }
    else
    {
        targetChip->FitsB.blob    = const_cast<void *>(fitsData);
        targetChip->FitsB.bloblen = totalBytes;
        snprintf(targetChip->FitsB.format, MAXINDIBLOBFMT, ".%s", targetChip->getImageExtension());
    }

    targetChip->FitsB.size = totalBytes;
    targetChip->FitsBP.s   = IPS_OK;

    if (sendImage)
    {
#ifdef HAVE_WEBSOCKET
        if (HasWebSocket() && WebSocketS[WEBSOCKET_ENABLED].s == ISS_ON)
        {
            auto start = std::chrono::high_resolution_clock::now();

            // Send format/size/..etc first later
            wsServer.send_text(std::string(targetChip->FitsB.format));
            wsServer.send_binary(targetChip->FitsB.blob, targetChip->FitsB.bloblen);

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> diff = end - start;
            LOGF_DEBUG("Websocket transfer took %g seconds", diff.count());
        }
        else
#endif
        {
            auto start = std::chrono::high_resolution_clock::now();
            IDSetBLOB(&targetChip->FitsBP, nullptr);
            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> diff = end - start;
            LOGF_DEBUG("BLOB transfer took %g seconds", diff.count());
        }
    }

    if (compressedData)
        delete [] compressedData;

    DEBUG(Logger::DBG_DEBUG, "Upload complete");

    return true;
}

bool CCD::processFastExposure(CCDChip * targetChip)
{
    // If fast exposure is on, let's immediately take another capture
    if (FastExposureToggleS[INDI_ENABLED].s == ISS_ON)
    {
        targetChip->setExposureComplete();
        double duration = targetChip->getExposureDuration();

        // Check fast exposure count
        if (FastExposureCountN[0].value > 1)
        {
            if (UploadS[UPLOAD_LOCAL].s != ISS_ON)
            {
                if (FastExposureCountNP.s != IPS_BUSY)
                {
                    FastExposureToggleStartup = std::chrono::system_clock::now();
                }
                else
                {
                    auto end = std::chrono::system_clock::now();

                    m_UploadTime = (std::chrono::duration_cast<std::chrono::milliseconds>(end - FastExposureToggleStartup)).count() / 1000.0 -
                                   duration;
                    LOGF_DEBUG("Image download and upload/save took %.3f seconds.", m_UploadTime);

                    FastExposureToggleStartup = end;
                }
            }

            FastExposureCountNP.s = IPS_BUSY;
            FastExposureCountN[0].value--;
            IDSetNumber(&FastExposureCountNP, nullptr);

            if (UploadS[UPLOAD_LOCAL].s == ISS_ON || m_UploadTime < duration)
            {
                if (StartExposure(duration))
                    PrimaryCCD.ImageExposureNP.s = IPS_BUSY;
                else
                    PrimaryCCD.ImageExposureNP.s = IPS_ALERT;
                if (duration * 1000 < getCurrentPollingPeriod())
                    setCurrentPollingPeriod(duration * 950);
            }
            else
            {
                LOGF_ERROR("Rapid exposure not possible since upload time is %.2f seconds while exposure time is %.2f seconds.",
                           m_UploadTime,
                           duration);
                PrimaryCCD.ImageExposureNP.s = IPS_ALERT;
                IDSetNumber(&PrimaryCCD.ImageExposureNP, nullptr);
                FastExposureCountN[0].value = 1;
                FastExposureCountNP.s = IPS_IDLE;
                IDSetNumber(&FastExposureCountNP, nullptr);
                m_UploadTime = 0;
                return false;
            }
        }
        else
        {
            m_UploadTime = 0;
            FastExposureCountNP.s = IPS_IDLE;
            IDSetNumber(&FastExposureCountNP, nullptr);
        }
    }

    return true;
}

void CCD::SetCCDParams(int x, int y, int bpp, float xf, float yf)
{
    PrimaryCCD.setResolution(x, y);
    PrimaryCCD.setFrame(0, 0, x, y);
    if (CanBin())
        PrimaryCCD.setBin(1, 1);
    PrimaryCCD.setPixelSize(xf, yf);
    PrimaryCCD.setBPP(bpp);
}

void CCD::SetGuiderParams(int x, int y, int bpp, float xf, float yf)
{
    capability |= CCD_HAS_GUIDE_HEAD;

    GuideCCD.setResolution(x, y);
    GuideCCD.setFrame(0, 0, x, y);
    GuideCCD.setPixelSize(xf, yf);
    GuideCCD.setBPP(bpp);
}

bool CCD::saveConfigItems(FILE * fp)
{
    DefaultDevice::saveConfigItems(fp);

    IUSaveConfigText(fp, &ActiveDeviceTP);
    IUSaveConfigSwitch(fp, &UploadSP);
    IUSaveConfigText(fp, &UploadSettingsTP);
    IUSaveConfigSwitch(fp, &TelescopeTypeSP);
    IUSaveConfigSwitch(fp, &FastExposureToggleSP);

    IUSaveConfigSwitch(fp, &PrimaryCCD.CompressSP);

    IUSaveConfigSwitch(fp, &CaptureFormatSP);
    IUSaveConfigSwitch(fp, &EncodeFormatSP);

    if (HasCooler())
        IUSaveConfigNumber(fp, &TemperatureRampNP);

    if (HasGuideHead())
    {
        IUSaveConfigSwitch(fp, &GuideCCD.CompressSP);
        IUSaveConfigNumber(fp, &GuideCCD.ImageBinNP);
    }

    if (CanSubFrame() && PrimaryCCD.ImageFrameN[2].value > 0)
        IUSaveConfigNumber(fp, &PrimaryCCD.ImageFrameNP);

    if (CanBin())
        IUSaveConfigNumber(fp, &PrimaryCCD.ImageBinNP);

    if (HasBayer())
        IUSaveConfigText(fp, &BayerTP);

    if (HasStreaming())
        Streamer->saveConfigItems(fp);

    if (HasDSP())
        DSP->saveConfigItems(fp);

    return true;
}

IPState CCD::GuideNorth(uint32_t ms)
{
    INDI_UNUSED(ms);
    LOG_ERROR("The CCD does not support guiding.");
    return IPS_ALERT;
}

IPState CCD::GuideSouth(uint32_t ms)
{
    INDI_UNUSED(ms);
    LOG_ERROR("The CCD does not support guiding.");
    return IPS_ALERT;
}

IPState CCD::GuideEast(uint32_t ms)
{
    INDI_UNUSED(ms);
    LOG_ERROR("The CCD does not support guiding.");
    return IPS_ALERT;
}

IPState CCD::GuideWest(uint32_t ms)
{
    INDI_UNUSED(ms);
    LOG_ERROR("The CCD does not support guiding.");
    return IPS_ALERT;
}

void CCD::getMinMax(double * min, double * max, CCDChip * targetChip)
{
    int ind         = 0, i, j;
    int imageHeight = targetChip->getSubH() / targetChip->getBinY();
    int imageWidth  = targetChip->getSubW() / targetChip->getBinX();
    double lmin = 0, lmax = 0;

    switch (targetChip->getBPP())
    {
        case 8:
        {
            uint8_t * imageBuffer = targetChip->getFrameBuffer();
            lmin = lmax = imageBuffer[0];

            for (i = 0; i < imageHeight; i++)
                for (j = 0; j < imageWidth; j++)
                {
                    ind = (i * imageWidth) + j;
                    if (imageBuffer[ind] < lmin)
                        lmin = imageBuffer[ind];
                    else if (imageBuffer[ind] > lmax)
                        lmax = imageBuffer[ind];
                }
        }
        break;

        case 16:
        {
            uint16_t * imageBuffer = reinterpret_cast<uint16_t*>(targetChip->getFrameBuffer());
            lmin = lmax = imageBuffer[0];

            for (i = 0; i < imageHeight; i++)
                for (j = 0; j < imageWidth; j++)
                {
                    ind = (i * imageWidth) + j;
                    if (imageBuffer[ind] < lmin)
                        lmin = imageBuffer[ind];
                    else if (imageBuffer[ind] > lmax)
                        lmax = imageBuffer[ind];
                }
        }
        break;

        case 32:
        {
            uint32_t * imageBuffer = reinterpret_cast<uint32_t*>(targetChip->getFrameBuffer());
            lmin = lmax = imageBuffer[0];

            for (i = 0; i < imageHeight; i++)
                for (j = 0; j < imageWidth; j++)
                {
                    ind = (i * imageWidth) + j;
                    if (imageBuffer[ind] < lmin)
                        lmin = imageBuffer[ind];
                    else if (imageBuffer[ind] > lmax)
                        lmax = imageBuffer[ind];
                }
        }
        break;
    }
    *min = lmin;
    *max = lmax;
}

std::string regex_replace_compat(const std::string &input, const std::string &pattern, const std::string &replace)
{
    std::stringstream s;
    std::regex_replace(std::ostreambuf_iterator<char>(s), input.begin(), input.end(), std::regex(pattern), replace);
    return s.str();
}

int CCD::getFileIndex(const char * dir, const char * prefix, const char * ext)
{
    INDI_UNUSED(ext);

    DIR * dpdf = nullptr;
    struct dirent * epdf = nullptr;
    std::vector<std::string> files = std::vector<std::string>();

    std::string prefixIndex = prefix;
    prefixIndex             = regex_replace_compat(prefixIndex, "_ISO8601", "");
    prefixIndex             = regex_replace_compat(prefixIndex, "_XXX", "");

    // Create directory if does not exist
    struct stat st;

    if (stat(dir, &st) == -1)
    {
        if (errno == ENOENT)
        {
            DEBUGF(Logger::DBG_DEBUG, "Creating directory %s...", dir);
            if (INDI::mkpath(dir, 0755) == -1)
                LOGF_ERROR("Error creating directory %s (%s)", dir, strerror(errno));
        }
        else
        {
            LOGF_ERROR("Couldn't stat directory %s: %s", dir, strerror(errno));
            return -1;
        }
    }

    dpdf = opendir(dir);
    if (dpdf != nullptr)
    {
        while ((epdf = readdir(dpdf)))
        {
            if (strstr(epdf->d_name, prefixIndex.c_str()))
                files.push_back(epdf->d_name);
        }
    }
    else
    {
        closedir(dpdf);
        return -1;
    }
    int maxIndex = 0;

    for (uint32_t i = 0; i < files.size(); i++)
    {
        int index = -1;

        std::string file  = files.at(i);
        std::size_t start = file.find_last_of("_");
        std::size_t end   = file.find_last_of(".");
        if (start != std::string::npos)
        {
            index = atoi(file.substr(start + 1, end).c_str());
            if (index > maxIndex)
                maxIndex = index;
        }
    }

    closedir(dpdf);
    return (maxIndex + 1);
}

void CCD::GuideComplete(INDI_EQ_AXIS axis)
{
    GuiderInterface::GuideComplete(axis);
}

bool CCD::StartStreaming()
{
    LOG_ERROR("Streaming is not supported.");
    return false;
}

bool CCD::StopStreaming()
{
    LOG_ERROR("Streaming is not supported.");
    return false;
}

#ifdef HAVE_WEBSOCKET
void CCD::wsThreadHelper(void * context)
{
    static_cast<CCD *>(context)->wsThreadEntry();
}

void CCD::wsThreadEntry()
{
    wsServer.run();
}
#endif

/////////////////////////////////////////////////////////////////////////////////////////
///
/////////////////////////////////////////////////////////////////////////////////////////
void CCD::checkTemperatureTarget()
{
    if (TemperatureNP.s == IPS_BUSY)
    {
        if (std::abs(m_TargetTemperature - TemperatureN[0].value) <= TemperatureRampNP[RAMP_THRESHOLD].value)
        {
            TemperatureNP.s = IPS_OK;
            m_TemperatureCheckTimer.stop();
            IDSetNumber(&TemperatureNP, nullptr);
        }
        // If we are beyond a minute, check for next step
        else if (m_TemperatureElapsedTimer.elapsed() >= 60000)
        {
            double nextTemperature = 0;
            // Going down
            if (m_TargetTemperature < TemperatureN[0].value)
            {
                nextTemperature = std::max(m_TargetTemperature, TemperatureN[0].value - TemperatureRampNP[RAMP_SLOPE].value);
            }
            // Going up
            else
            {
                nextTemperature = std::min(m_TargetTemperature, TemperatureN[0].value + TemperatureRampNP[RAMP_SLOPE].value);
            }

            m_TemperatureElapsedTimer.restart();
            SetTemperature(nextTemperature);
        }
    }
}

void CCD::addCaptureFormat(const CaptureFormat &format)
{
    // Avoid duplicates.
    auto pos = std::find_if(m_CaptureFormats.begin(), m_CaptureFormats.end(), [format](auto & oneFormat)
    {
        return format.name == oneFormat.name;
    });
    if (pos != m_CaptureFormats.end())
        return;

    // Add NEW format.
    auto count = CaptureFormatSP.size();
    CaptureFormatSP.resize(count + 1);
    // Format is ON if the label matches the configuration label OR if there is no configuration saved and isDefault is true.
    const bool isOn = (format.label == m_ConfigCaptureFormatLabel) || (m_ConfigCaptureFormatLabel.empty() && format.isDefault);
    CaptureFormatSP[count].fill(format.name.c_str(), format.label.c_str(), isOn ? ISS_ON : ISS_OFF);
    m_CaptureFormats.push_back(format);
}

bool CCD::SetCaptureFormat(uint8_t index)
{
    INDI_UNUSED(index);
    return true;
}

}
