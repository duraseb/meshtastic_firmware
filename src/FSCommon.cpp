/**
 * @file FSCommon.cpp
 * @brief This file contains functions for common filesystem operations such as copying, renaming, listing and deleting files and
 * directories.
 *
 * The functions in this file are used to perform common filesystem operations such as copying, renaming, listing and deleting
 * files and directories. These functions are used in the Meshtastic-device project to manage files and directories on the
 * device's filesystem.
 *
 */
#include "FSCommon.h"
#include "SPILock.h"
#include "configuration.h"

#ifdef ARCH_ESP32
#include "esp_task_wdt.h"
#endif

// Software SPI is used by MUI so disable SD card here until it's also implemented
#if defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI)
#include <SD.h>
#include <SPI.h>

#ifdef SDCARD_USE_SPI1
SPIClass SPI_HSPI(HSPI);
#define SDHandler SPI_HSPI
#else
#define SDHandler SPI
#endif

#ifndef SD_SPI_FREQUENCY
#define SD_SPI_FREQUENCY 4000000U
#endif

#endif // HAS_SDCARD

/**
 * @brief Copies a file from one location to another.
 *
 * @param from The path of the source file.
 * @param to The path of the destination file.
 * @return true if the file was successfully copied, false otherwise.
 */
bool copyFile(const char *from, const char *to)
{
#ifdef FSCom
    // take SPI Lock
    concurrency::LockGuard g(spiLock);
    unsigned char cbuffer[16];

    File f1 = FSCom.open(from, FILE_O_READ);
    if (!f1) {
        LOG_ERROR("Failed to open source file %s", from);
        return false;
    }

    File f2 = FSCom.open(to, FILE_O_WRITE);
    if (!f2) {
        LOG_ERROR("Failed to open destination file %s", to);
        return false;
    }

    while (f1.available() > 0) {
        byte i = f1.read(cbuffer, 16);
        f2.write(cbuffer, i);
    }

    f2.flush();
    f2.close();
    f1.close();
    return true;
#endif
}

/**
 * Renames a file from pathFrom to pathTo.
 *
 * @param pathFrom The original path of the file.
 * @param pathTo The new path of the file.
 *
 * @return True if the file was successfully renamed, false otherwise.
 */
bool renameFile(const char *pathFrom, const char *pathTo)
{
#ifdef FSCom

#ifdef ARCH_ESP32
    // take SPI Lock
    spiLock->lock();
    // rename was fixed for ESP32 IDF LittleFS in April
    bool result = FSCom.rename(pathFrom, pathTo);
    spiLock->unlock();
    return result;
#else
    // copyFile does its own locking.
    if (copyFile(pathFrom, pathTo) && FSCom.remove(pathFrom)) {
        return true;
    } else {
        return false;
    }
#endif

#endif
}

#include <vector>

/**
 * @brief Get the list of files in a directory.
 *
 * This function returns a list of files in a directory. The list includes the full path of each file.
 * We can't use SPILOCK here because of recursion. Callers of this function should use SPILOCK.
 *
 * @param dirname The name of the directory.
 * @param levels The number of levels of subdirectories to list.
 * @return A vector of strings containing the full path of each file in the directory.
 */
std::vector<meshtastic_FileInfo> getFiles(const char *dirname, uint8_t levels)
{
    std::vector<meshtastic_FileInfo> filenames = {};
#ifdef FSCom
    File root = FSCom.open(dirname, FILE_O_READ);
    if (!root)
        return filenames;
    if (!root.isDirectory())
        return filenames;

    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory() && !String(file.name()).endsWith(".")) {
            if (levels) {
#ifdef ARCH_ESP32
                std::vector<meshtastic_FileInfo> subDirFilenames = getFiles(file.path(), levels - 1);
#else
                std::vector<meshtastic_FileInfo> subDirFilenames = getFiles(file.name(), levels - 1);
#endif
                filenames.insert(filenames.end(), subDirFilenames.begin(), subDirFilenames.end());
                file.close();
            }
        } else {
            meshtastic_FileInfo fileInfo = {"", static_cast<uint32_t>(file.size())};
#ifdef ARCH_ESP32
            strcpy(fileInfo.file_name, file.path());
#else
            strcpy(fileInfo.file_name, file.name());
#endif
            if (!String(fileInfo.file_name).endsWith(".")) {
                filenames.push_back(fileInfo);
            }
            file.close();
        }
        file = root.openNextFile();
    }
    root.close();
#endif
    return filenames;
}

/**
 * Lists the contents of a directory.
 * We can't use SPILOCK here because of recursion. Callers of this function should use SPILOCK.
 *
 * @param dirname The name of the directory to list.
 * @param levels The number of levels of subdirectories to list.
 * @param del Whether or not to delete the contents of the directory after listing.
 */
void listDir(const char *dirname, uint8_t levels, bool del)
{
#ifdef FSCom
#if (defined(ARCH_ESP32) || defined(ARCH_RP2040) || defined(ARCH_PORTDUINO))
    char buffer[255];
#endif
    File root = FSCom.open(dirname, FILE_O_READ);
    if (!root) {
        return;
    }
    if (!root.isDirectory()) {
        return;
    }

    File file = root.openNextFile();
    while (
        file &&
        file.name()[0]) { // This file.name() check is a workaround for a bug in the Adafruit LittleFS nrf52 glue (see issue 4395)
        if (file.isDirectory() && !String(file.name()).endsWith(".")) {
            if (levels) {
#ifdef ARCH_ESP32
                listDir(file.path(), levels - 1, del);
                if (del) {
                    LOG_DEBUG("Remove %s", file.path());
                    strncpy(buffer, file.path(), sizeof(buffer));
                    file.close();
                    FSCom.rmdir(buffer);
                } else {
                    file.close();
                }
#elif (defined(ARCH_RP2040) || defined(ARCH_PORTDUINO))
                listDir(file.name(), levels - 1, del);
                if (del) {
                    LOG_DEBUG("Remove %s", file.name());
                    strncpy(buffer, file.name(), sizeof(buffer));
                    file.close();
                    FSCom.rmdir(buffer);
                } else {
                    file.close();
                }
#else
                LOG_DEBUG(" %s (directory)", file.name());
                listDir(file.name(), levels - 1, del);
                file.close();
#endif
            }
        } else {
#ifdef ARCH_ESP32
            if (del) {
                LOG_DEBUG("Delete %s", file.path());
                strncpy(buffer, file.path(), sizeof(buffer));
                file.close();
                FSCom.remove(buffer);
            } else {
                LOG_DEBUG(" %s (%i Bytes)", file.path(), file.size());
                file.close();
            }
#elif (defined(ARCH_RP2040) || defined(ARCH_PORTDUINO))
            if (del) {
                LOG_DEBUG("Delete %s", file.name());
                strncpy(buffer, file.name(), sizeof(buffer));
                file.close();
                FSCom.remove(buffer);
            } else {
                LOG_DEBUG(" %s (%i Bytes)", file.name(), file.size());
                file.close();
            }
#else
            LOG_DEBUG("   %s (%i Bytes)", file.name(), file.size());
            file.close();
#endif
        }
        file = root.openNextFile();
    }
#ifdef ARCH_ESP32
    if (del) {
        LOG_DEBUG("Remove %s", root.path());
        strncpy(buffer, root.path(), sizeof(buffer));
        root.close();
        FSCom.rmdir(buffer);
    } else {
        root.close();
    }
#elif (defined(ARCH_RP2040) || defined(ARCH_PORTDUINO))
    if (del) {
        LOG_DEBUG("Remove %s", root.name());
        strncpy(buffer, root.name(), sizeof(buffer));
        root.close();
        FSCom.rmdir(buffer);
    } else {
        root.close();
    }
#else
    root.close();
#endif
#endif
}

/**
 * @brief Removes a directory and all its contents.
 *
 * This function recursively removes a directory and all its contents, including subdirectories and files.
 *
 * @param dirname The name of the directory to remove.
 */
void rmDir(const char *dirname)
{
#ifdef FSCom

#if (defined(ARCH_ESP32) || defined(ARCH_RP2040) || defined(ARCH_PORTDUINO))
    listDir(dirname, 10, true);
#elif defined(ARCH_NRF52)
    // nRF52 implementation of LittleFS has a recursive delete function
    FSCom.rmdir_r(dirname);
#endif

#endif
}

/**
 * Some platforms (nrf52) might need to do an extra step before FSBegin().
 */
__attribute__((weak, noinline)) void preFSBegin() {}

/**
 * Emergency cleanup for /alerts/ directory if filesystem is critically full.
 * This runs BEFORE normal boot to prevent crashes due to filesystem exhaustion.
 * 
 * When the filesystem is >90% full with many alert files, we aggressively delete
 * files to free up space for config saves and other operations.
 */
void emergencyAlertCleanup()
{
#ifdef FSCom
#if defined(ARCH_ESP32)
    // Check if filesystem is critically full (>90% used)
    size_t totalBytes = FSCom.totalBytes();
    size_t usedBytes = FSCom.usedBytes();
    
    if (totalBytes == 0) return;
    
    size_t freeBytes = totalBytes - usedBytes;
    size_t criticalThreshold = totalBytes / 10;  // 10% of total = critical threshold
    
    if (freeBytes > criticalThreshold) {
        return;  // Filesystem has enough space, no emergency cleanup needed
    }
    
    LOG_WARN("Filesystem critically full (%d/%d bytes, %d%% used) - running emergency /alerts/ cleanup",
             usedBytes, totalBytes, (usedBytes * 100) / totalBytes);
    
    // Check if /alerts/ directory exists
    File alertsDir = FSCom.open("/alerts", FILE_O_READ);
    if (!alertsDir || !alertsDir.isDirectory()) {
        if (alertsDir) alertsDir.close();
        LOG_DEBUG("No /alerts/ directory found");
        return;
    }
    
    // AGGRESSIVE cleanup: delete ALL alert files in a single pass
    // This is simpler and faster than trying to find "oldest" files
    // We need to free space quickly to allow boot to continue
    int filesDeleted = 0;
    int filesProcessed = 0;
    size_t targetFreeBytes = totalBytes / 2;
    
    File file = alertsDir.openNextFile();
    while (file) {
        // Feed watchdog every 5 files to prevent timeout
        filesProcessed++;
        if (filesProcessed % 5 == 0) {
            esp_task_wdt_reset();
        }
        
        if (!file.isDirectory()) {
            String filename = file.name();
            file.close();  // Close before delete
            
            // Delete any .tmp or .bin file in /alerts/
            if (filename.endsWith(".tmp") || filename.endsWith(".bin")) {
                String fullPath = String("/alerts/") + filename;
                if (FSCom.remove(fullPath.c_str())) {
                    filesDeleted++;
                    
                    // Check if we've freed enough space (check every 10 deletes)
                    if (filesDeleted % 10 == 0) {
                        freeBytes = FSCom.totalBytes() - FSCom.usedBytes();
                        if (freeBytes >= targetFreeBytes) {
                            LOG_INFO("Emergency cleanup: freed enough space after %d files", filesDeleted);
                            break;
                        }
                        // Feed watchdog after space check
                        esp_task_wdt_reset();
                    }
                }
            }
        } else {
            file.close();
        }
        
        esp_task_wdt_reset();  // Feed watchdog before opening next file
        file = alertsDir.openNextFile();
    }
    alertsDir.close();
    
    // Final watchdog feed
    esp_task_wdt_reset();
    
    if (filesDeleted > 0) {
        usedBytes = FSCom.usedBytes();
        LOG_WARN("Emergency cleanup: deleted %d files", filesDeleted);
        LOG_INFO("Filesystem now: %d/%d bytes (%d%% used)",
                 usedBytes, totalBytes, (usedBytes * 100) / totalBytes);
    } else {
        LOG_WARN("Emergency cleanup: no files deleted, filesystem still full");
    }
#endif
#endif
}

void fsInit()
{
#ifdef FSCom
    concurrency::LockGuard g(spiLock);
    preFSBegin();
    if (!FSBegin()) {
        LOG_ERROR("Filesystem mount failed");
        // assert(0); This auto-formats the partition, so no need to fail here.
    }
    
    // Run emergency cleanup BEFORE listing files (to prevent crash from full FS)
    emergencyAlertCleanup();
    
#if defined(ARCH_ESP32)
    LOG_DEBUG("Filesystem: %d/%d Bytes (%d%% used)", 
              FSCom.usedBytes(), FSCom.totalBytes(),
              (FSCom.usedBytes() * 100) / FSCom.totalBytes());
#else
    LOG_DEBUG("Filesystem files:");
    listDir("/", 10);
#endif
#endif
}

/**
 * Initializes the SD card and mounts the file system.
 */
void setupSDCard()
{
#if defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI)
    concurrency::LockGuard g(spiLock);
    SDHandler.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    if (!SD.begin(SDCARD_CS, SDHandler, SD_SPI_FREQUENCY)) {
        LOG_DEBUG("No SD_MMC card detected");
        return;
    }
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        LOG_DEBUG("No SD_MMC card attached");
        return;
    }
    LOG_DEBUG("SD_MMC Card Type: ");
    if (cardType == CARD_MMC) {
        LOG_DEBUG("MMC");
    } else if (cardType == CARD_SD) {
        LOG_DEBUG("SDSC");
    } else if (cardType == CARD_SDHC) {
        LOG_DEBUG("SDHC");
    } else {
        LOG_DEBUG("UNKNOWN");
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    LOG_DEBUG("SD Card Size: %lu MB", (uint32_t)cardSize);
    LOG_DEBUG("Total space: %lu MB", (uint32_t)(SD.totalBytes() / (1024 * 1024)));
    LOG_DEBUG("Used space: %lu MB", (uint32_t)(SD.usedBytes() / (1024 * 1024)));
#endif
}