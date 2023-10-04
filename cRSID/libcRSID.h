// cRSID lightweight RealSID (integer-only) library-header (with API-calls) by Hermit (Mihaly Horvath)

#ifndef LIBCRSID_HEADER
#define LIBCRSID_HEADER //used  to prevent double inclusion of this header-file


enum cRSID_Specifications { CRSID_SIDCOUNT_MAX=3, CRSID_CIACOUNT=2 };
enum cRSID_StatusCodes    { CRSID_STATUS_OK=0, CRSID_ERROR_INIT=-1, CRSID_ERROR_LOAD=-2 };


typedef struct cRSID_SIDinstance cRSID_SIDinstance;


// Main API functions (mainly in libcRSID.c)
cRSID_C64instance* cRSID_init           (unsigned short samplerate, unsigned short buflen); //init emulation objects and sound
static inline signed short cRSID_generateSample (cRSID_C64instance* C64); //in host/audio.c, calculate a single sample

void               cRSID_createSIDchip (cRSID_C64instance* C64, cRSID_SIDinstance* SID, unsigned short model, unsigned short baseaddress);
void               cRSID_initSIDchip   (cRSID_SIDinstance* SID);
void               cRSID_emulateADSRs  (cRSID_SIDinstance *SID, char cycles);
int                cRSID_emulateWaves  (cRSID_SIDinstance* SID);
void               cRSID_generateSound (cRSID_C64instance* C64, unsigned char* buf, unsigned short len);


struct cRSID_SIDinstance {
 //SID-chip data:
 cRSID_C64instance* C64;           //reference to the containing C64
 unsigned short     ChipModel;     //values: 8580 / 6581
 //ADSR-related:
 unsigned char      ADSRstate[15];
 unsigned short     RateCounter[15];
 unsigned char      EnvelopeCounter[15];
 unsigned char      ExponentCounter[15];
 //Wave-related:
 int                PhaseAccu[15];       //28bit precision instead of 24bit
 int                PrevPhaseAccu[15];   //(integerized ClockRatio fractionals, WebSID has similar solution)
 unsigned char      SyncSourceMSBrise;
 unsigned int       RingSourceMSB;
 unsigned int       NoiseLFSR[15];
 unsigned int       PrevWavGenOut[15];
 unsigned char      PrevWavData[15];
 //Filter-related:
 int                PrevLowPass;
 int                PrevBandPass;
 //Output-stage:
 signed int         PrevVolume; //lowpass-filtered version of Volume-band register
                                //
 unsigned char      BasePtr[28];
};


struct cRSID_C64instance {
 //platform-related:
 unsigned short    SampleRate;
 //C64-machine related:
 unsigned int      CPUfrequency;
 unsigned short    SampleClockRatio; //ratio of CPU-clock and samplerate

 //Hardware-elements:
 cRSID_SIDinstance SID;
};


#endif //LIBCRSID_HEADER
