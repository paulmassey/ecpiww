#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>

#include <unistd.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <ctype.h>
#include <energycam/ecpiww.h>
#include <energycam/wmbusext.h>


void Colour(int8_t c, bool cr) {
    printf("%c[%dm",0x1B,(c>0) ? (30+c) : c);
    if(cr)
        printf("\n");
}

//Log Reading with date info to CSV File
int Log2CSVFile(const char *path,  double Value) {
    FILE    *hFile;
    uint32_t FileSize = 0;


    time_t t = time(NULL);
    struct tm tm = *localtime(&t);

    if ((hFile = fopen(path, "rb")) != NULL) {
        fseek(hFile, 0L, SEEK_END);
        FileSize = ftell(hFile);
        fseek(hFile, 0L, SEEK_SET);
        fclose(hFile);
    }

    if ((hFile = fopen(path, "a")) != NULL) {
        if (FileSize == 0)  //start a new file with Header
            fprintf(hFile, "Date, Value \n");
        fprintf(hFile,"%d-%02d-%02d %02d:%02d, %.1f\n", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, Value);
       fclose(hFile);
    } else
        return APIERROR;

    return APIOK;
}

static struct termios orig_term_attr;


static int iTime;
static int iMinute;

int IsNewSecond(int iS) {
    int CurTime;
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    CurTime = tm.tm_hour*60*60+tm.tm_min*60+tm.tm_sec;
    if (iS > 0)
        CurTime = CurTime/iS;

    if (CurTime != iTime) {
        iTime = CurTime;
        return 1;
    }
    return 0;
}

int IsNewMinute(void) {
    int CurTime;
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    CurTime = tm.tm_hour*60+tm.tm_min;

    if (CurTime != iMinute) {
        iMinute = CurTime;
        return 1;
    }
    return 0;
}

void IntroShowParam(void) {
    printf("   \n");
    Colour(62,false);
    printf("################################################################\n");
    printf("## ecpiww - EnergyCam/wM-Bus Stick on raspberry Pi/cubieboard ##\n");
    printf("################################################################\n");
    Colour(0,true);
    printf("   Commandline options:\n");
    printf("   ./ecpiww -f /home/user/ecdata -p 0 -m S\n");
    printf("   -f <dir>           : save readings to <dir>/<wM-Bus Ident>.dat\n");
    printf("   -l VZ              : log to (VZ)Volkszaehler, (XML) XMLFile, (CSV) CSV File \n");
    printf("   -p 0               : Portnumber 0 -> /dev/ttyUSB0     (as alternativ to -d)\n");
    printf("   -d </path/to/dev>  : e.g. /dev/tty.usbserial-27019A35 (as alternativ to -p)\n");
    printf("   -m S               : S2 mode \n");
    printf("   -i                 : show detailed infos \n\n");
}

void ErrorAndExit(const char *info) {
    Colour(PRINTF_RED, false);
    printf("%s", info);
    Colour(0, true);
    exit(0);
}

unsigned int CalcUIntBCD(  unsigned int ident) {
    int32_t identNumBCD=0;
    #define MAXIDENTLEN 12
    uint8_t  identchar[MAXIDENTLEN];
    memset(identchar,0,MAXIDENTLEN*sizeof(uint8_t));
    sprintf((char *)identchar, "%08d", ident);
    uint32_t uiMul = 1;
    uint8_t  uiX   = 0;
    uint8_t  uiLen = strlen((char const*)identchar);

    for(uiX=0; uiX < uiLen;uiX++) {
        identNumBCD += (identchar[uiLen-1-uiX] - '0')*uiMul;
        uiMul = uiMul*16;
    }
    return identNumBCD;
}

void DisplayListofMeters(int iMax, pecwMBUSMeter ecpiwwMeter) {
    int iX,iI;

    if(iMax == 0) printf("\nNo Meters defined.\n");
    else {
        iI=0;
        for(iX=0; iX<iMax; iX++) {
            if( 0 != ecpiwwMeter[iX].manufacturerID)
               iI++;
        }
        printf("\nList of active Meters (%d defined):\n", iI);
     }

    for(iX=0;iX<iMax;iX++) {
        if( 0 != ecpiwwMeter[iX].manufacturerID) {
            printf("Meter#%d : Manufactor = 0x%02X\n", iX+1, ecpiwwMeter[iX].manufacturerID);
            printf("Meter#%d : Ident      = 0x%08X\n", iX+1, ecpiwwMeter[iX].ident);
            printf("Meter#%d : Type       = 0x%02X\n", iX+1, ecpiwwMeter[iX].type);
            printf("Meter#%d : Version    = 0x%02X\n", iX+1, ecpiwwMeter[iX].version);
            printf("Meter#%d : Key        = 0x", iX+1);
            for(iI = 0; iI<AES_KEYLENGHT_IN_BYTES; iI++)
                printf("%02X",ecpiwwMeter[iX].key[iI]);
            printf("\n");
        }
    }
}

void UpdateMetersonStick(unsigned long handle, uint16_t stick, int iMax, pecwMBUSMeter ecpiwwMeter, uint16_t infoflag) {
    int iX;

    for(iX=0; iX<MAXMETER; iX++)
        wMBus_RemoveMeter(iX);

    for(iX=0; iX<iMax; iX++) {
        if( 0 != ecpiwwMeter[iX].manufacturerID)
            wMBus_AddMeter(handle, stick, iX, &ecpiwwMeter[iX], infoflag);
    }
}

#define XMLBUFFER (1*1024*1024)
unsigned int Log2XMLFile(const char *path, double Reading, ecMBUSData *rfData) {
    char szBuf[250];
    FILE    *hFile;
    unsigned char*  pXMLIN = NULL;
    unsigned char*  pXMLTop,*pXMLMem = NULL;
    unsigned char*  pXML;
    unsigned int   dwSize   = 0;
    unsigned int   dwSizeIn = 0;
    char  CurrentTime[250];

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);

    if ( (hFile = fopen(path, "rb")) != NULL ) {
        fseek(hFile, 0L, SEEK_END);
        dwSizeIn = ftell(hFile);
        fseek(hFile, 0L, SEEK_SET);

        pXMLIN = (unsigned char*) malloc(dwSizeIn+4096);
        memset(pXMLIN, 0, sizeof(unsigned char)*(dwSizeIn+4096));
        if(NULL == pXMLIN) {
            printf("Log2XMLFile - malloc failed\n");
            return 0;
        }

        fread(pXMLIN, dwSizeIn, 1, hFile);
        fclose(hFile);

        pXMLTop = (unsigned char *)strstr((const char*)pXMLIN, "<ENERGYCAMOCR>\n"); //search on start
        if(pXMLTop) {
            pXMLTop += strlen("<ENERGYCAMOCR>\n");
            pXMLMem = (unsigned char *) malloc(max(4*XMLBUFFER, XMLBUFFER+dwSizeIn));
            if(NULL == pXMLMem) {
                printf("Log2XMLFile - malloc %d failed \n", max(4*XMLBUFFER, XMLBUFFER+dwSizeIn));
                return 0;
            }
            memset(pXMLMem, 0, sizeof(unsigned char)*max(4*XMLBUFFER,XMLBUFFER+dwSizeIn));
            pXML = pXMLMem;
            dwSize=0;
            dwSizeIn -= pXMLTop-pXMLIN;

            sprintf(szBuf, "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n");
            memcpy(pXML, szBuf, strlen(szBuf)); dwSize+=strlen(szBuf); pXML+=strlen(szBuf);

            sprintf(szBuf, "<ENERGYCAMOCR>\n");
            memcpy(pXML, szBuf, strlen(szBuf)); dwSize+=strlen(szBuf); pXML+=strlen(szBuf);
        }
    } else {
        //new File
        pXMLMem = (unsigned char *) malloc(XMLBUFFER);
        memset(pXMLMem, 0, sizeof(unsigned char)*XMLBUFFER);
        pXML = pXMLMem;
        dwSize=0;

        sprintf(szBuf, "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n");
        memcpy(pXML, szBuf, strlen(szBuf)); dwSize+=strlen(szBuf); pXML+=strlen(szBuf);

        sprintf(szBuf, "<ENERGYCAMOCR>\n");
        memcpy(pXML, szBuf, strlen(szBuf)); dwSize+=strlen(szBuf); pXML+=strlen(szBuf);
    }

    if(pXMLMem) {
        sprintf(szBuf, "<OCR>\n");
        memcpy(pXML, szBuf, strlen(szBuf)); dwSize+=strlen(szBuf); pXML+=strlen(szBuf);

        sprintf(CurrentTime, "%02d.%02d.%d %02d:%02d:%02d", tm.tm_mday,tm.tm_mon+1, tm.tm_year+1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
        sprintf(szBuf, "<Date>%s</Date>\n", CurrentTime); memcpy(pXML, szBuf,strlen(szBuf)); dwSize+=strlen(szBuf); pXML+=strlen(szBuf);

        sprintf(szBuf, "<Reading>%.1f</Reading>\n", Reading); memcpy(pXML, szBuf, strlen(szBuf)); dwSize+=strlen(szBuf); pXML+=strlen(szBuf);
        if(NULL != rfData) {
            sprintf(szBuf, "<RSSI>%d</RSSI>\n",                 rfData->rssiDBm);    memcpy(pXML, szBuf, strlen(szBuf)); dwSize+=strlen(szBuf); pXML+=strlen(szBuf);
            sprintf(szBuf, "<Pic>%d</Pic>\n",                   rfData->utcnt_pic);  memcpy(pXML, szBuf, strlen(szBuf)); dwSize+=strlen(szBuf); pXML+=strlen(szBuf);
            sprintf(szBuf, "<Tx>%d</Tx>\n",                     rfData->utcnt_tx);   memcpy(pXML, szBuf, strlen(szBuf)); dwSize+=strlen(szBuf); pXML+=strlen(szBuf);
            sprintf(szBuf, "<ConfigWord>%d</ConfigWord>\n",     rfData->configWord); memcpy(pXML, szBuf, strlen(szBuf)); dwSize+=strlen(szBuf); pXML+=strlen(szBuf);
            sprintf(szBuf, "<wMBUSStatus>%d</wMBUSStatus>\n",   rfData->status);     memcpy(pXML, szBuf, strlen(szBuf)); dwSize+=strlen(szBuf); pXML+=strlen(szBuf);
        }
        sprintf(szBuf,"</OCR>\n");memcpy(pXML,szBuf,strlen(szBuf));dwSize+=strlen(szBuf);pXML+=strlen(szBuf);

        if(dwSizeIn>0) {
            memcpy(pXML, pXMLTop, dwSizeIn);
        } else {
            sprintf(szBuf, "</ENERGYCAMOCR>\n"); memcpy(pXML, szBuf, strlen(szBuf)); dwSize+=strlen(szBuf); pXML+=strlen(szBuf);
        }

        if ( (hFile = fopen(path, "wb")) != NULL ) {
            fwrite(pXMLMem, dwSizeIn+dwSize, 1, hFile);
            fclose(hFile);
            chmod(path, 0666);
        } else {
            fprintf(stderr, "Cannot write to >%s<\n", path);
        }
    }

    if(pXMLMem) free(pXMLMem);
    if(pXMLIN)  free(pXMLIN);

    return true;
}

//Log Reading with date info to CSV File
int Log2File(char *DataPath, uint16_t mode, uint16_t meterindex, uint16_t infoflag, float metervalue, ecMBUSData *rfData, uint32_t ident) {
    char  param[  _MAX_PATH];
    char  datFile[_MAX_PATH];
    FILE  *hDatFile;
    time_t t;
    struct tm curtime;

    if(infoflag > SILENTMODE) printf("mode %d, datapath %s, meter value %f \n", mode, DataPath, metervalue);

    switch(mode) {
        case LOGTOCSV : if (strlen(DataPath) == 0) {
                            sprintf(param, "/var/www/ecpiww/data/ecpiwwM%d.csv", meterindex+1);
                        } else {
                            sprintf(param, "%s/ecpiwwM%d.csv", DataPath, meterindex+1);
                        }
                        Log2CSVFile(param, metervalue); //log kWh
                        return APIOK;
                        break;
        case LOGTOXML : if (strlen(DataPath) == 0) {
                            getcwd(param, sizeof(param));
                            sprintf(param, "%s/%08X.xml", param, ident);
                        } else {
                            sprintf(param, "%s/%08X.xml", DataPath, ident);
                        }
                        Log2XMLFile(param, metervalue, rfData); //log kWh
                        return APIOK;
                        break;
        case LOGTOVZ :  if( access( "add2vz.sh", F_OK ) != -1 ) {
                            memset(param, '\0', sizeof(FILENAME_MAX));
                            sprintf(param, "./add2vz.sh %d %ld ", meterindex+1, (long int)(metervalue*1000));
                            int ret=system(param);
                            t = time(NULL);
                            curtime = *localtime(&t);

                            if(0x100 == ret) {
                                if(infoflag > SILENTMODE) printf("%02d:%02d Calling %s \n", curtime.tm_hour, curtime.tm_min, param);
                            } else {
                                if(infoflag > SILENTMODE) printf("%02d:%02d Calling %s returned with 0x%X\n", curtime.tm_hour, curtime.tm_min, param, ret);
                            }
                        }
                        return APIOK;
                        break;
        case LOGTODAT:
                        if (strlen(DataPath) == 0)  {
                            getcwd(param, sizeof(param));
                            sprintf(datFile, "%s/%08X.dat", param, ident);
                        } else {
                            sprintf(datFile, "%s/%08X.dat", DataPath, ident);
                        }
                        if ((hDatFile = fopen(datFile, "wb")) != NULL) {
                            fprintf(hDatFile, "%.1f\n",  metervalue);
                            fclose(hDatFile);
                        }
                        return APIOK;
                        break;
    }
    return APIERROR;
}

//support commandline
int parseparam(int argc, char *argv[], char *filepath, uint16_t *infoflag, uint16_t *Port, char *devicepath, uint16_t *Mode, uint16_t *LogMode) {
    int c;

    if((NULL == LogMode) || (NULL == infoflag) || (NULL == Port)  || (NULL == Mode) ) return 0;

    opterr = 0;
    while ((c = getopt (argc, argv, "f:hil:m:p:d:x")) != -1) {
        switch (c) {
            case 'f':
                if (NULL != optarg) {
                    strcpy(filepath,optarg);
                }
                break;
            case 'i':
                *infoflag = SHOWDETAILS;
                break;
            case 'l':
                if (NULL != optarg) {
                    if(0 == strcmp("VZ",  optarg)) *LogMode=LOGTOVZ;
                    if(0 == strcmp("XML", optarg)) *LogMode=LOGTOXML;
                    if(0 == strcmp("DAT", optarg)) *LogMode=LOGTODAT;
                }
                break;
            case 'p':
                if (NULL != optarg) {
                    *Port = atoi(optarg);
                }
                break;
            case 'd':
                if (NULL != optarg) {
                    strcpy(devicepath,optarg);
                }
                break;
            case 'm':
                if (NULL != optarg) {
                    if(0 == strcmp("S", optarg)) *Mode=RADIOS2;
                }
                break;
            case 'h':
                IntroShowParam();
                exit (0);
                break;
            case '?':
                if (optopt == 'f')
                    fprintf (stderr, "Option -%c requires an argument.\n", optopt);
                else if (isprint (optopt))
                    fprintf (stderr, "Unknown option `-%c'.\n", optopt);
                else
                    fprintf (stderr, "Unknown option character `\\x%x'.\n", optopt);
                return 1;
            default:
                abort ();
        }
    }
    return 0;
}

//////////////////////////////////////////////
int main(int argc, char *argv[]) {
    signed char key    = -1;
    int      iCheck = 0;
    int      iX;
    int      iK;
    char     KeyInput[_MAX_PATH];
    char     Key[3];
    char     CommandlineDatPath[_MAX_PATH];
    double   csvValue;
    int      Meters = 0;
    uint8_t  ReturnValue;
    FILE    *hDatFile;

    uint16_t InfoFlag = SILENTMODE;
    uint16_t Port = 0;
    char     DevicePath[_MAX_PATH] = "";
    uint16_t Mode = RADIOT2;
    uint16_t LogMode = LOGTOCSV;
    uint16_t wMBUSStick = iM871AIdentifier;

    char     comDeviceName[100];
    int      hStick;

    ecwMBUSMeter ecpiwwMeter[MAXMETER];
    memset(ecpiwwMeter, 0, MAXMETER*sizeof(ecwMBUSMeter));

    memset(CommandlineDatPath, 0, _MAX_PATH*sizeof(char));

    if(argc > 1)
      parseparam(argc, argv, CommandlineDatPath, &InfoFlag, &Port, DevicePath, &Mode, &LogMode);

    //read config back
    if ((hDatFile = fopen("meter.dat", "rb")) != NULL) {
        Meters = fread((void*)ecpiwwMeter, sizeof(ecwMBUSMeter), MAXMETER, hDatFile);
        fclose(hDatFile);
    }

   //open wM-Bus Stick #1
    wMBUSStick = iM871AIdentifier;
    if (strlen(DevicePath))
        sprintf(comDeviceName, "%s", DevicePath);
    else
        sprintf(comDeviceName, "/dev/ttyUSB%d", Port);

    hStick = wMBus_OpenDevice(comDeviceName, wMBUSStick);

    if(hStick <= 0) {
         ErrorAndExit("wM-Bus Stick not found\n");
    }

    if((iM871AIdentifier == wMBUSStick) && (APIOK == wMBus_GetStickId(hStick, wMBUSStick, &ReturnValue, InfoFlag)) && (iM871AIdentifier == ReturnValue)) {
        if(InfoFlag > SILENTMODE) {
            printf("IMST iM871A Stick found\n");
        }
    } else {
        wMBus_CloseDevice(hStick, wMBUSStick);
        ErrorAndExit("wM-Bus Stick not found\n");
    }

    if(APIOK == wMBus_GetRadioMode(hStick, wMBUSStick, &ReturnValue, InfoFlag)) {
        if(InfoFlag > SILENTMODE) {
            printf("wM-BUS %s Mode\n", (ReturnValue == RADIOT2) ? "T2" : "S2");
        }
        if (ReturnValue != Mode)
           wMBus_SwitchMode(hStick, wMBUSStick, (uint8_t) Mode, InfoFlag);
    } else {
        wMBus_CloseDevice(hStick, wMBUSStick);
        ErrorAndExit("wM-Bus Stick not found\n");
    }

    wMBus_InitDevice(hStick, wMBUSStick, InfoFlag);

    UpdateMetersonStick(hStick, wMBUSStick, Meters, ecpiwwMeter, InfoFlag);

    IsNewMinute();

    while (true) {
        usleep(500*1000);       //sleep 500ms


        if (IsNewMinute()) { //check whether there are new data from the EnergyCams
            if(wMBus_GetMeterDataList() > 0) {
                iCheck = 0;
                for(iX=0; iX<Meters; iX++) {
                    if((0x01<<iX) & wMBus_GetMeterDataList()) {
                        ecMBUSData RFData;
                        int iMul=1;
                        int iDiv=1;
                        wMBus_GetData4Meter(iX, &RFData);

                        if(RFData.exp < 0) {  //GAS
                            for(iK=RFData.exp; iK<0; iK++)
                               iDiv=iDiv*10;
                            csvValue = ((double)RFData.value)/iDiv;
                        } else {
                            for(iK=0; iK<RFData.exp; iK++)
                                iMul=iMul*10;
                            csvValue = (double)RFData.value*iMul;
                        }
                        if(InfoFlag > SILENTMODE) {
                            Colour(PRINTF_GREEN, false);
                            printf("Meter #%d : %4.1f %s", iX+1, csvValue, (ecpiwwMeter[iX].type == METER_ELECTRICITY) ? "Wh" : "m'3");
                        }
                        if(ecpiwwMeter[iX].type == METER_ELECTRICITY)
                            csvValue = csvValue/1000.0;

                        if(InfoFlag > SILENTMODE) {
                            if((RFData.pktInfo & PACKET_WAS_ENCRYPTED)      ==  PACKET_WAS_ENCRYPTED)     printf(" Decryption OK");
                            if((RFData.pktInfo & PACKET_DECRYPTIONERROR)    ==  PACKET_DECRYPTIONERROR)   printf(" Decryption ERROR");
                            if((RFData.pktInfo & PACKET_WAS_NOT_ENCRYPTED)  ==  PACKET_WAS_NOT_ENCRYPTED) printf(" not encrypted");
                            if((RFData.pktInfo & PACKET_IS_ENCRYPTED)       ==  PACKET_IS_ENCRYPTED)      printf(" is encrypted");
                            printf(" RSSI=%i dbm, #%d \n", RFData.rssiDBm, RFData.accNo);
                            Colour(0,false);
                        }

                        Log2File(CommandlineDatPath, LogMode, iX, InfoFlag, csvValue, &RFData, ecpiwwMeter[iX].ident);

                    }
                }
            }
            else {
              if(InfoFlag > SILENTMODE) {
                  Colour(PRINTF_YELLOW, false);
                  printf("%02d ", iCheck);
                  Colour(0, false);
                  if((++iCheck % 20) == 0) printf("\n");
                }
            }
        }
 
    } // end while

    if(hStick >0) wMBus_CloseDevice(hStick, wMBUSStick);

    //save Meter config to file
    if(Meters > 0) {
        if ((hDatFile = fopen("meter.dat", "wb")) != NULL) {
            fwrite((void*)ecpiwwMeter, sizeof(ecwMBUSMeter), MAXMETER, hDatFile);
            fclose(hDatFile);
        }
    }
    return 0;
}
