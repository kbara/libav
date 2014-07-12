# NOTE: the REALAUDIO tests need to use the RA demuxer once
# it is available
FATE_SAMPLES_REALAUDIO-$(call DEMDEC, RM, RA_144) += fate-ra3-144
fate-ra3-144: CMD = framecrc -i $(TARGET_SAMPLES)/realaudio/ra3.ra

FATE_SAMPLES_REALAUDIO-$(call DEMDEC, RM, RA_288) += fate-ra4-28_8
fate-ra4-28_8: CMD = framecrc -i $(TARGET_SAMPLES)/realaudio/ra4-28_8.ra

FATE_SAMPLES_REALAUDIO-$(call DEMDEC, RM, SIPR) += fate-ra4-sipr5k
fate-ra4-sipr5k: CMD = framecrc -i $(TARGET_SAMPLES)/realaudio/ra4-sipr5k.ra

FATE_SAMPLES_REALMEDIA_AUDIO-$(call DEMDEC, RM, RA_144) += fate-ra-144
fate-ra-144: CMD = md5 -i $(TARGET_SAMPLES)/real/ra3_in_rm_file.rm -f s16le

FATE_SAMPLES_REALMEDIA_AUDIO-$(call DEMDEC, RM, RA_288) += fate-ra-288
fate-ra-288: CMD = pcm -i $(TARGET_SAMPLES)/real/ra_288.rm
fate-ra-288: CMP = oneoff
fate-ra-288: REF = $(SAMPLES)/real/ra_288.pcm
fate-ra-288: FUZZ = 2

FATE_SAMPLES_REALMEDIA_AUDIO-$(call DEMDEC, RM, COOK) += fate-ra-cook
fate-ra-cook: CMD = pcm -i $(TARGET_SAMPLES)/real/ra_cook.rm
fate-ra-cook: CMP = oneoff
fate-ra-cook: REF = $(SAMPLES)/real/ra_cook.pcm

FATE_SAMPLES_REALMEDIA_VIDEO-$(call DEMDEC, RM, RV30) += fate-rv30
fate-rv30: CMD = framecrc -flags +bitexact -idct simple -i $(TARGET_SAMPLES)/real/rv30.rm -an

FATE_SAMPLES_REALMEDIA_VIDEO-$(call DEMDEC, RM, RV40) += fate-rv40
fate-rv40: CMD = framecrc -i $(TARGET_SAMPLES)/real/spygames-2MB.rmvb -t 10 -an -vsync 0

FATE_SIPR += fate-sipr-5k0
fate-sipr-5k0: CMD = pcm -i $(TARGET_SAMPLES)/sipr/sipr_5k0.rm
fate-sipr-5k0: REF = $(SAMPLES)/sipr/sipr_5k0.pcm

FATE_SIPR += fate-sipr-6k5
fate-sipr-6k5: CMD = pcm -i $(TARGET_SAMPLES)/sipr/sipr_6k5.rm
fate-sipr-6k5: REF = $(SAMPLES)/sipr/sipr_6k5.pcm

FATE_SIPR += fate-sipr-8k5
fate-sipr-8k5: CMD = pcm -i $(TARGET_SAMPLES)/sipr/sipr_8k5.rm
fate-sipr-8k5: REF = $(SAMPLES)/sipr/sipr_8k5.pcm

FATE_SIPR += fate-sipr-16k
fate-sipr-16k: CMD = pcm -i $(TARGET_SAMPLES)/sipr/sipr_16k.rm
fate-sipr-16k: REF = $(SAMPLES)/sipr/sipr_16k.pcm

$(FATE_SIPR): CMP = oneoff

FATE_SAMPLES_REALMEDIA_AUDIO-$(call DEMDEC, RM, SIPR) += $(FATE_SIPR)
fate-sipr: $(FATE_SIPR)
fate-realaudio: $(FATE_SAMPLES_REALAUDIO-yes)
fate-realmedia-audio: $(FATE_SAMPLES_REALMEDIA_AUDIO-yes)
fate-realmedia-video: $(FATE_SAMPLES_REALMEDIA_VIDEO-yes)
fate-realmedia: fate-realmedia-audio fate-realmedia-video
# TODO: what about fate-ralf and fate-rangecoder?
FATE_SAMPLES_AVCONV += $(FATE_SAMPLES_REALMEDIA_AUDIO-yes) $(FATE_SAMPLES_REALMEDIA_VIDEO-yes) $(FATE_SAMPLES_REALAUDIO-yes)

