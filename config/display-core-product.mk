PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/snapalloc/resources/camera_alignments.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/camera_alignments.json
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/snapalloc/resources/cpu_alignments.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/cpu_alignments.json
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/snapalloc/resources/default_alignments.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/default_alignments.json
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/snapalloc/resources/display_alignments.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/display_alignments.json
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/snapalloc/resources/formats.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/formats.json
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/snapalloc/resources/graphics_alignments.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/graphics_alignments.json
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/snapalloc/resources/ubwc_alignments.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/ubwc_alignments.json
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/snapalloc/resources/video_alignments.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/video_alignments.json

#QDCM calibration json file for nt37801 panel
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC.json
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_nt37801_amoled_video_mode_dsi_csot_panel_with_DSC.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_nt37801_amoled_video_mode_dsi_csot_panel_with_DSC.json
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.json
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_nt37801_amoled_video_mode_dsi_csot_panel_with_DSC_CPHY.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_nt37801_amoled_video_mode_dsi_csot_panel_with_DSC_CPHY.json

#QDCM calibration json file for Sharp panel
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_Sharp_qhd_cmd_mode_dsi_panel.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_Sharp_qhd_cmd_mode_dsi_panel.json
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_Sharp_qhd_video_mode_dsi_panel.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_Sharp_qhd_video_mode_dsi_panel.json

#QDCM calibration json file for vtdr6130 panel
ifeq ($(TARGET_BOARD_PLATFORM),malabar)
	PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_vtdr6130_amoled_cmd_mode_dsi_visionox_panel_with_DSC_malabar.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_vtdr6130_amoled_cmd_mode_dsi_visionox_panel_with_DSC.json
	PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_vtdr6130_amoled_video_mode_dsi_visionox_panel_with_DSC_malabar.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_vtdr6130_amoled_video_mode_dsi_visionox_panel_with_DSC.json
else
	PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_vtdr6130_amoled_cmd_mode_dsi_visionox_panel_with_DSC.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_vtdr6130_amoled_cmd_mode_dsi_visionox_panel_with_DSC.json
	PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_vtdr6130_amoled_video_mode_dsi_visionox_panel_with_DSC.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_vtdr6130_amoled_video_mode_dsi_visionox_panel_with_DSC.json
endif
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_vtdr6130_amoled_qsync_cmd_mode_dsi_visionox_panel_with_DSC.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_vtdr6130_amoled_qsync_cmd_mode_dsi_visionox_panel_with_DSC.json
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_vtdr6130_amoled_qsync_video_mode_dsi_visionox_panel_with_DSC.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_vtdr6130_amoled_qsync_video_mode_dsi_visionox_panel_with_DSC.json
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_vtdr6130_amoled_cmd_mode_dsi_visionox_panel_without_dsc.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_vtdr6130_amoled_cmd_mode_dsi_visionox_panel_without_dsc.json

#QDCM calibration json file for ft8726 panel
ifeq ($(TARGET_BOARD_PLATFORM),malabar)
	PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_ft8726_lcd_video_mode_dsi_focaltech_panel_with_DSC_malabar.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_ft8726_lcd_video_mode_dsi_focaltech_panel_with_DSC.json
else
	PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_ft8726_lcd_video_mode_dsi_focaltech_panel_with_DSC.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_ft8726_lcd_video_mode_dsi_focaltech_panel_with_DSC.json
endif

#QDCM calibration json file for nt37802 video PSR amoled VHM panels
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_nt37802_video_PSR_amoled_VHM_120hz_dsi_panel_with_DSC.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_nt37802_video_PSR_amoled_VHM_120hz_dsi_panel_with_DSC.json

#QDCM calibration json file for RaonTech panel
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/qdcm_calib_data_RaonTech_Non-FSC_mode_video_1440x1440@60_mode_dsi_panel.json:$(TARGET_COPY_OUT_VENDOR)/etc/display/qdcm_calib_data_RaonTech_Non-FSC_mode_video_1440x1440@60_mode_dsi_panel.json

#Backlight calibration xml file for nt37801 amoled panels
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/backlight_calib_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display/backlight_calib_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.xml
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/backlight_calib_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display/backlight_calib_nt37801_amoled_video_mode_dsi_csot_panel_with_DSC_CPHY.xml
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/backlight_calib_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display/backlight_calib_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC.xml
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/backlight_calib_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display/backlight_calib_nt37801_amoled_video_mode_dsi_csot_panel_with_DSC.xml

#Backlight calibration xml file for Sharp panel
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/backlight_calib_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display/backlight_calib_Sharp_qhd_cmd_mode_dsi_panel.xml
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/backlight_calib_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display/backlight_calib_Sharp_qhd_video_mode_dsi_panel.xml

#Backlight calibration xml file for vtdr6130 amoled panels
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/backlight_calib_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display/backlight_calib_vtdr6130_amoled_cmd_mode_dsi_visionox_panel_with_DSC.xml
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/backlight_calib_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display/backlight_calib_vtdr6130_amoled_video_mode_dsi_visionox_panel_with_DSC.xml
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/backlight_calib_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display/backlight_calib_vtdr6130_amoled_qsync_cmd_mode_dsi_visionox_panel_with_DSC.xml
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/backlight_calib_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display/backlight_calib_vtdr6130_amoled_qsync_video_mode_dsi_visionox_panel_with_DSC.xml

#Backlight calibration xml file for nt37802 video PSR amoled VHM panels
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/backlight_calib_nt37801_amoled_cmd_mode_dsi_csot_panel_with_DSC_CPHY.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display/backlight_calib_nt37802_video_PSR_amoled_VHM_120hz_dsi_panel_with_DSC.xml

#SDR Dimming config file for nt37801, display id is 4630946916234099603
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/display_id_4630946916234099603.xml:$(TARGET_COPY_OUT_VENDOR)/etc/displayconfig/display_id_4630946916234099603.xml

#SDR Dimming config file for vtdr6130, display id is 4630947039571902851
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/display_id_4630946916234099603.xml:$(TARGET_COPY_OUT_VENDOR)/etc/displayconfig/display_id_4630947039571902851.xml
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/display_id_4630946916234099603.xml:$(TARGET_COPY_OUT_VENDOR)/etc/displayconfig/display_id_4630947039571902850.xml

#SDR Dimming config file for Sharp, display id is 4630947075271898515
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/display_id_4630946916234099603.xml:$(TARGET_COPY_OUT_VENDOR)/etc/displayconfig/display_id_4630947075271898515.xml

#SDR Dimming config file for nt37802 video PSR VHM, display id is 4630946850534658451
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/display_id_4630946916234099603.xml:$(TARGET_COPY_OUT_VENDOR)/etc/displayconfig/display_id_4630946850534658451.xml

PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/sdm_display_resolution_extn.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display/sdm_display_resolution_extn.xml

ifneq ($(TARGET_HAS_LOW_RAM),true)
#Multi-stc libraries config xml file
PRODUCT_COPY_FILES += vendor/qcom/opensource/display-core/config/snapdragon_color_libs_config.xml:$(TARGET_COPY_OUT_VENDOR)/etc/snapdragon_color_libs_config.xml
endif

PRIVACY_REGIONS_OFFSETS_XML_PATH := vendor/qcom/opensource/display-core/config
PRODUCT_COPY_FILES += $(PRIVACY_REGIONS_OFFSETS_XML_PATH)/privacy_regions_offsets.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display/privacy_regions_offsets.xml
