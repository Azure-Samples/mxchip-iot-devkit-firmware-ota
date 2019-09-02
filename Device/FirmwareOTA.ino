// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. 
// To get started please visit https://microsoft.github.io/azure-iot-developer-kit/docs/projects/connect-iot-hub?utm_source=ArduinoExtension&utm_medium=ReleaseNote&utm_campaign=VSCode
#include "AZ3166WiFi.h"
#include "AzureIotHub.h"
#include "DevKitMQTTClient.h"
#include "DevKitOTAUtils.h"
#include "OTAFirmwareUpdate.h"
#include "SystemTime.h"

static const char* currentFirmwareVersion = "1.0.0";

static bool hasWifi = false;

static bool enableOTA = true;
static const FW_INFO* fwInfo = NULL;
static MAP_HANDLE OTAStatusMap = NULL;

//////////////////////////////////////////////////////////////////////////////////////////////////////////
// Utilities
// Initialize Wi-Fi
static void InitWifi()
{
  Screen.print(2, "Connecting...");
  
  if (WiFi.begin() == WL_CONNECTED)
  {
    IPAddress ip = WiFi.localIP();
    Screen.print(1, ip.get_address());
    hasWifi = true;
    Screen.print(2, "Running... \r\n");
  }
  else
  {
    hasWifi = false;
    Screen.print(1, "No Wi-Fi\r\n ");
  }
}

// Report the OTA update status to Azure
static void ReportOTAStatus(const char* currentFwVersion, const char* pendingFwVersion, const char* fwUpdateStatus, const char* fwUpdateSubstatus, const char* lastFwUpdateStartTime, const char* lastFwUpdateEndTime)
{
  OTAStatusMap = Map_Create(NULL);
  Map_Add(OTAStatusMap, "type", "IoTDevKit"); // The type of the firmware
  if (currentFwVersion)
  {
    Map_Add(OTAStatusMap, OTA_CURRENT_FW_VERSION, currentFwVersion);
  }
  if (pendingFwVersion)
  {
    Map_Add(OTAStatusMap, OTA_PENDING_FW_VERSION, pendingFwVersion);
  }
  if (fwUpdateStatus)
  {
    Map_Add(OTAStatusMap, OTA_FW_UPDATE_STATUS, fwUpdateStatus);
  }
  if (fwUpdateSubstatus)
  {
    Map_Add(OTAStatusMap, OTA_FW_UPDATE_SUBSTATUS, fwUpdateSubstatus);
  }
  if (lastFwUpdateStartTime)
  {
    Map_Add(OTAStatusMap, OTA_LAST_FW_UPDATE_STARTTIME, lastFwUpdateStartTime);
  }
  if (lastFwUpdateEndTime)
  {
    Map_Add(OTAStatusMap, OTA_LAST_FW_UPDATE_ENDTIME, lastFwUpdateEndTime);
  }
  IoTHubClient_ReportOTAStatus(OTAStatusMap);
  Map_Destroy(OTAStatusMap);
  DevKitMQTTClient_Check();
}
// Enter a failed state, print failed message and report status
static void OTAUpdateFailed(const char* failedMsg)
{
  ReportOTAStatus(currentFirmwareVersion, fwInfo ? fwInfo->fwVersion : "", OTA_STATUS_ERROR, failedMsg, "", "");
  enableOTA = false;
  Screen.print(1, "OTA failed:");
  Screen.print(2, failedMsg);
  Screen.print(3, " ");
  LogInfo("Failed to update firmware %s: %s, disable OTA update.", fwInfo ? fwInfo->fwVersion : "<unknown>", failedMsg);
}

// Check for new firmware
static void CheckNewFirmware(void)
{
  if (!enableOTA)
  {
    return;
  }
    
  // Check if there has new firmware info.
  fwInfo = IoTHubClient_GetLatestFwInfo();
  if (fwInfo == NULL)
  {
    // No firmware update info
    return;
  }
  
  if (fwInfo->fwVersion == NULL || fwInfo->fwPackageURI == NULL)
  {
    // Disable 
    LogInfo("Invalid new firmware infomation retrieved, disable OTA update.");
    enableOTA = false;
    return;
  }

  // Check if the URL is https as we require it for safety purpose.
  if (strlen(fwInfo->fwPackageURI) < 6 || (strncmp("https:", fwInfo->fwPackageURI, 6) != 0))
  {
    // Report error status, URINotHTTPS
    OTAUpdateFailed("URINotHTTPS");
    return;
  }

  // Check if this is a new version.
  if (IoTHubClient_FwVersionCompare(fwInfo->fwVersion, currentFirmwareVersion) <= 0)
  {
    // The firmware version from cloud <= the running firmware version
    return;
  }

  // New firemware
  Screen.print(1, "New firmware:");
  Screen.print(2, fwInfo->fwVersion);
  LogInfo("New firmware is available: %s.", fwInfo->fwVersion);
  
  Screen.print(3, " downloading...");
  LogInfo(">> Downloading from %s...", fwInfo->fwPackageURI);
  // Report downloading status.
  char startTimeStr[30];
  time_t t = time(NULL);
  strftime(startTimeStr, 30, "%Y-%m-%dT%H:%M:%S.0000000Z", gmtime(&t));
  ReportOTAStatus(currentFirmwareVersion, fwInfo->fwVersion, OTA_STATUS_DOWNLOADING, fwInfo->fwPackageURI, startTimeStr, "");
  
  // Close IoT Hub Client to release the TLS resource for firmware download.
  DevKitMQTTClient_Close();
  // Download the firmware. This can be customized according to the board type.
  uint16_t checksum = 0;
  int fwSize = OTADownloadFirmware(fwInfo->fwPackageURI, &checksum);

  // Reopen the IoT Hub Client for reporting status.
  DevKitMQTTClient_Init(true);

  // Check result
  if (fwSize == 0 || fwSize == -1)
  {
    // Report error status, DownloadFailed
    OTAUpdateFailed("DownloadFailed");
    return;
  }
  else if (fwSize == -2)
  {
    // Report error status, DeviceError
    OTAUpdateFailed("DeviceError");
    return;
  }
  else if (fwSize != fwInfo->fwSize)
  {
    // Report error status, FileSizeNotMatch
    OTAUpdateFailed("FileSizeNotMatch");
    return;
  }
  
  Screen.print(3, " Finished.");
  LogInfo(">> Finished download.");

  // CRC check
  if (fwInfo->fwPackageCheckValue != NULL)
  {
    Screen.print(3, " verifying...");

    if (checksum == strtoul(fwInfo->fwPackageCheckValue, NULL, 16))
    {
      Screen.print(3, " passed.");
      LogInfo(">> CRC check passed.");
    }
    else 
    {
      // Report error status, VerifyFailed
      OTAUpdateFailed("VerifyFailed");
      Screen.print(3, " CRC failed.");
      return;
    }
  }

  // Applying
  if (OTAApplyNewFirmware(fwSize, checksum) != 0)
  {
    // Report error status, ApplyFirmwareFailed
    OTAUpdateFailed("ApplyFirmwareFailed");
    Screen.print(3, " Apply failed.");
    return;
  }
  // Report status
  char endTimeStr[30];
  t = time(NULL);
  strftime(endTimeStr, 30, "%Y-%m-%dT%H:%M:%S.0000000Z", gmtime(&t));
  ReportOTAStatus(currentFirmwareVersion, fwInfo->fwVersion, OTA_STATUS_APPLYING, "", startTimeStr, endTimeStr);
  
  // Counting down and reboot
  Screen.clean();
  Screen.print(0, "Reboot system");
  LogInfo("Reboot system to apply the new firmware:");
  char msg[2] = { 0 };
  for (int i = 0; i < 5; i++)
  {
    msg[0] = '0' + 5 - i;
    Screen.print(2, msg);
    LogInfo(msg);
    delay(1000);
  }
  
  // Reboot system to apply the firmware.
  SystemReboot();
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
// Arduino sketch
void setup()
{
  Screen.init();
  Screen.print(0, "IoT DevKit");
  Screen.print(2, "Initializing...");
  
  Screen.print(3, " > Serial");
  Serial.begin(115200);

  // Initialize the WiFi module
  Screen.print(3, " > WiFi");
  hasWifi = false;
  InitWifi();
  if (!hasWifi)
  {
    return;
  }
  
  // Print out the version
  Screen.print(1, "FW version:");
  Screen.print(2, currentFirmwareVersion);
  Screen.print(3, " checking...");

  // Initialize MQTT client
  DevKitMQTTClient_SetOption(OPTION_MINI_SOLUTION_NAME, "FirmwareOTA");
  DevKitMQTTClient_Init(true);
  
  LogInfo("FirmwareOTA demo: %s", currentFirmwareVersion);
  // Report OTA status
  ReportOTAStatus(currentFirmwareVersion, "", OTA_STATUS_CURRENT, "", NULL, NULL);
}

void loop()
{
  if (hasWifi)
  {
    DevKitMQTTClient_Check();

    // Check for new firmware
    CheckNewFirmware();

    LogInfo("No new firmware...");
  }
  delay(1000);
}
