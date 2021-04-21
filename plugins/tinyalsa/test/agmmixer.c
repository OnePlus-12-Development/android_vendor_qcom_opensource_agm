/*
** Copyright (c) 2019, 2021, The Linux Foundation. All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above
**     copyright notice, this list of conditions and the following
**     disclaimer in the documentation and/or other materials provided
**     with the distribution.
**   * Neither the name of The Linux Foundation nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
** WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
** ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
** BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
** BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
** WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
** OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
** IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**/

#include <errno.h>
#include <expat.h>
#include <tinyalsa/asoundlib.h>
#include <sound/asound.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>

#include <agm/agm_api.h>
#include "agmmixer.h"

#define EVENT_ID_DETECTION_ENGINE_GENERIC_INFO 0x0800104F

#define PARAM_ID_DETECTION_ENGINE_GENERIC_EVENT_CFG 0x0800104E
#define PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT            0x08001024

enum pcm_channel_map
{
   PCM_CHANNEL_L = 1,
   PCM_CHANNEL_R = 2,
   PCM_CHANNEL_C = 3,
   PCM_CHANNEL_LS = 4,
   PCM_CHANNEL_RS = 5,
   PCM_CHANNEL_LFE = 6,
   PCM_CHANNEL_CS = 7,
   PCM_CHANNEL_CB = PCM_CHANNEL_CS,
   PCM_CHANNEL_LB = 8,
   PCM_CHANNEL_RB = 9,
};
/* Payload of the PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT parameter in the
 Media Format Converter Module. Following this will be the variable payload for channel_map. */
struct param_id_mfc_output_media_fmt_t
{
   int32_t sampling_rate;
   int16_t bit_width;
   int16_t num_channels;
   uint16_t channel_type[0];
}__attribute__((packed));

struct apm_module_param_data_t
{
   uint32_t module_instance_id;
   uint32_t param_id;
   uint32_t param_size;
   uint32_t error_code;
};

struct detection_engine_generic_event_cfg {
    uint32_t event_mode;
};


struct gsl_module_id_info_entry {
	uint32_t module_id; /**< module id */
	uint32_t module_iid; /**< globally unique module instance id */
};

/**
 * Structure mapping the tag_id to module info (mid and miid)
 */
struct gsl_tag_module_info_entry {
	uint32_t tag_id; /**< tag id of the module */
	uint32_t num_modules; /**< number of modules matching the tag_id */
	struct gsl_module_id_info_entry module_entry[0]; /**< module list */
};

struct gsl_tag_module_info {
	uint32_t num_tags; /**< number of tags */
	struct gsl_tag_module_info_entry tag_module_entry[0];
	/**< variable payload of type struct gsl_tag_module_info_entry */
};

#define PADDING_8BYTE_ALIGN(x)  ((((x) + 7) & 7) ^ 7)

static unsigned int  bits_to_alsa_format(unsigned int bits)
{
    switch (bits) {
    case 32:
        return SNDRV_PCM_FORMAT_S32_LE;
    case 8:
        return SNDRV_PCM_FORMAT_S8;
    case 24:
        return SNDRV_PCM_FORMAT_S24_3LE;
    default:
    case 16:
        return SNDRV_PCM_FORMAT_S16_LE;
    };
}

void start_tag(void *userdata, const XML_Char *tag_name, const XML_Char **attr)
{
    struct device_config *config = (struct device_config *)userdata;

    if (strncmp(tag_name, "device", strlen("device")) != 0)
        return;

    if (strcmp(attr[0], "name") != 0) {
        printf("name not found\n");
        return;
    }

    if (strcmp(attr[2], "rate") != 0) {
        printf("rate not found\n");
        return;
    }

    if (strcmp(attr[4], "ch") != 0) {
        printf("channels not found\n");
        return;
    }

    if (strcmp(attr[6], "bits") != 0) {
        printf("bits not found");
        return;
    }

    if (strncmp(config->name, attr[1], sizeof(config->name)))
        return;

    config->rate = atoi(attr[3]);
    config->ch = atoi(attr[5]);
    config->bits = atoi(attr[7]);
}

int get_device_media_config(char* filename, char *intf_name, struct device_config *config)
{
    FILE *file = NULL;
    XML_Parser parser;
    int ret = 0;
    int bytes_read;
    void *buf = NULL;

    file = fopen(filename, "r");
    if (!file) {
        ret = -EINVAL;
        printf("Failed to open xml file name %s ret %d", filename, ret);
        goto done;
    }

    parser = XML_ParserCreate(NULL);
    if (!parser) {
        ret = -EINVAL;
        printf("Failed to create XML ret %d", ret);
        goto closeFile;
    }
    memset(config, 0, sizeof(*config));
    strlcpy(config->name, intf_name, sizeof(config->name));

    XML_SetUserData(parser, config);
    XML_SetElementHandler(parser, start_tag, NULL);

    while (1) {
        buf = XML_GetBuffer(parser, 1024);
        if (buf == NULL) {
            ret = -EINVAL;
            printf("XML_Getbuffer failed ret %d", ret);
            goto freeParser;
        }

        bytes_read = fread(buf, 1, 1024, file);
        if (bytes_read < 0) {
            ret = -EINVAL;
            printf("fread failed ret %d", ret);
            goto freeParser;
        }

        if (XML_ParseBuffer(parser, bytes_read, bytes_read == 0) == XML_STATUS_ERROR) {
            ret = -EINVAL;
            printf("XML ParseBuffer failed for %s file ret %d", filename, ret);
            goto freeParser;
        }
        if (bytes_read == 0 || config->rate != 0)
            break;
    }

    if (config->rate == 0) {
        ret = -EINVAL;
        printf("Entry not found\n");
    }
freeParser:
    XML_ParserFree(parser);
closeFile:
    fclose(file);
done:
    return ret;
}

int set_agm_device_media_config(struct mixer *mixer, unsigned int channels,
                                unsigned int rate, unsigned int bits, char *intf_name)
{
    char *control = "rate ch fmt";
    char *mixer_str;
    struct mixer_ctl *ctl;
    long media_config[4];
    int ctl_len = 0;
    int ret = 0;

    ctl_len = strlen(intf_name) + 1 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str) {
        printf("mixer_str calloc failed\n");
        return -ENOMEM;
    }
    snprintf(mixer_str, ctl_len, "%s %s", intf_name, control);

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(mixer_str);
        return ENOENT;
    }

    media_config[0] = rate;
    media_config[1] = channels;
    media_config[2] = bits_to_alsa_format(bits);
    media_config[3] = AGM_DATA_FORMAT_FIXED_POINT;

    ret = mixer_ctl_set_array(ctl, &media_config, sizeof(media_config)/sizeof(media_config[0]));
    free(mixer_str);
    return ret;
}

int connect_play_pcm_to_cap_pcm(struct mixer *mixer, unsigned int p_device, unsigned int c_device)
{
    char *pcm = "PCM";
    char *control = "loopback";
    char *mixer_str;
    struct mixer_ctl *ctl;
    int ctl_len = 0;
    char *val;
    int val_len = 0;
    int ret = 0;

    ctl_len = strlen(pcm) + 4 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str) {
        printf("mixer_str calloc failed\n");
        return -ENOMEM;
    }
    snprintf(mixer_str, ctl_len, "%s%d %s", pcm, c_device, control);

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(mixer_str);
        return ENOENT;
    }

    if (p_device < 0) {
        val = "ZERO";
    } else {
        val_len = strlen(pcm) + 4;
        val = calloc(1, val_len);
        if (!val) {
            printf("val calloc failed\n");
            free(mixer_str);
            return -ENOMEM;
        }
        snprintf(val, val_len, "%s%d", pcm, p_device);
    }

    ret = mixer_ctl_set_enum_by_string(ctl, val);
    free(mixer_str);
    if (p_device < 0)
        free(val);

    return ret;
}

int connect_agm_audio_intf_to_stream(struct mixer *mixer, unsigned int device, char *intf_name, enum stream_type stype, bool connect)
{
    char *stream = "PCM";
    char *control;
    char *mixer_str;
    struct mixer_ctl *ctl;
    int ctl_len = 0;
    int ret = 0;

    if (connect)
        control = "connect";
    else
        control = "disconnect";

    if (stype == STREAM_COMPRESS)
        stream = "COMPRESS";

    ctl_len = strlen(stream) + 4 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str) {
        printf("mixer_str calloc failed\n");
        return -ENOMEM;
    }
    snprintf(mixer_str, ctl_len, "%s%d %s", stream, device, control);

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(mixer_str);
        return ENOENT;
    }

    ret = mixer_ctl_set_enum_by_string(ctl, intf_name);
    free(mixer_str);
    return ret;
}

int agm_mixer_set_ecref_path(struct mixer *mixer, unsigned int device, enum stream_type stype, char *intf_name)
{
    char *stream = "PCM";
    char *control;
    char *mixer_str;
    struct mixer_ctl *ctl;
    int ctl_len = 0;
    int ret = 0;

    control = "echoReference";

    if (stype == STREAM_COMPRESS)
        stream = "COMPRESS";

    ctl_len = strlen(stream) + 4 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str) {
        printf("mixer_str calloc failed\n");
        return -ENOMEM;
    }
    snprintf(mixer_str, ctl_len, "%s%d %s", stream, device, control);

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(mixer_str);
        return ENOENT;
    }

    ret = mixer_ctl_set_enum_by_string(ctl, intf_name);
    free(mixer_str);
    return ret;
}

int set_agm_audio_intf_metadata(struct mixer *mixer, char *intf_name, enum dir d, int rate, int bitwidth)
{
    char *control = "metadata";
    struct mixer_ctl *ctl;
    char *mixer_str;
    struct agm_key_value *gkv = NULL, *ckv = NULL;
    struct prop_data *prop = NULL;
    uint8_t *metadata = NULL;
    uint32_t num_gkv = 1, num_ckv = 2, num_props = 0;
    uint32_t gkv_size, ckv_size, prop_size, ckv_index = 0;
    int ctl_len = 0, offset = 0;
    int ret = 0;

    gkv_size = num_gkv * sizeof(struct agm_key_value);
    ckv_size = num_ckv * sizeof(struct agm_key_value);
    prop_size = sizeof(struct prop_data) + (num_props * sizeof(uint32_t));

    metadata = calloc(1, sizeof(num_gkv) + sizeof(num_ckv) + gkv_size + ckv_size + prop_size);
    if (!metadata)
        return -ENOMEM;

    gkv = calloc(num_gkv, sizeof(struct agm_key_value));
    ckv = calloc(num_ckv, sizeof(struct agm_key_value));
    prop = calloc(1, prop_size);
    if (!gkv || !ckv || !prop) {
        if (ckv)
            free(ckv);
        if (gkv)
            free(gkv);
        free(metadata);
        return -ENOMEM;
    }

    if (d == PLAYBACK) {
        gkv[0].key = DEVICERX;
        gkv[0].value = SPEAKER;
    } else if (!strcmp(intf_name, "CODEC_DMA-LPAIF_VA-TX-0")) {
        gkv[0].key = DEVICETX;
        gkv[0].value = HANDSETMIC_VA;
    } else {
        gkv[0].key = DEVICETX;
        gkv[0].value = HANDSETMIC;
    }
    ckv[ckv_index].key = SAMPLINGRATE;
    ckv[ckv_index].value = rate;

    ckv_index++;
    ckv[ckv_index].key = BITWIDTH;
    ckv[ckv_index].value = bitwidth;

    prop->prop_id = 0;  //Update prop_id here
    prop->num_values = num_props;

    memcpy(metadata, &num_gkv, sizeof(num_gkv));
    offset += sizeof(num_gkv);
    memcpy(metadata + offset, gkv, gkv_size);
    offset += gkv_size;
    memcpy(metadata + offset, &num_ckv, sizeof(num_ckv));
    offset += sizeof(num_ckv);
    memcpy(metadata + offset, ckv, ckv_size);
    offset += ckv_size;
    memcpy(metadata + offset, prop, prop_size);

    ctl_len = strlen(intf_name) + 1 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str) {
        free(metadata);
        return -ENOMEM;
    }
    snprintf(mixer_str, ctl_len, "%s %s", intf_name, control);

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(gkv);
        free(ckv);
        free(prop);
        free(metadata);
        free(mixer_str);
        return ENOENT;
    }

    ret = mixer_ctl_set_array(ctl, metadata, sizeof(num_gkv) + sizeof(num_ckv) + gkv_size + ckv_size + prop_size);

    free(gkv);
    free(ckv);
    free(prop);
    free(metadata);
    free(mixer_str);
    return ret;
}

int set_agm_stream_metadata_type(struct mixer *mixer, int device, char *val, enum stream_type stype)
{
    char *stream = "PCM";
    char *control = "control";
    char *mixer_str;
    struct mixer_ctl *ctl;
    int ctl_len = 0,ret = 0;

    if (stype == STREAM_COMPRESS)
        stream = "COMPRESS";

    ctl_len = strlen(stream) + 4 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str) {
        printf("mixer_str calloc failed\n");
        return -ENOMEM;
    }
    snprintf(mixer_str, ctl_len, "%s%d %s", stream, device, control);

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(mixer_str);
        return ENOENT;
    }

    ret = mixer_ctl_set_enum_by_string(ctl, val);
    free(mixer_str);
    return ret;
}

void populateChannelMap(uint16_t *pcmChannel, uint8_t numChannel)
{
    if (numChannel == 1) {
        pcmChannel[0] = PCM_CHANNEL_C;
    } else if (numChannel == 2) {
        pcmChannel[0] = PCM_CHANNEL_L;
        pcmChannel[1] = PCM_CHANNEL_R;
    } else if (numChannel == 2) {
        pcmChannel[0] = PCM_CHANNEL_L;
        pcmChannel[1] = PCM_CHANNEL_R;
        pcmChannel[2] = PCM_CHANNEL_C;
    } else if (numChannel == 2) {
        pcmChannel[0] = PCM_CHANNEL_L;
        pcmChannel[1] = PCM_CHANNEL_R;
        pcmChannel[2] = PCM_CHANNEL_LB;
        pcmChannel[3] = PCM_CHANNEL_RB;
    }
}

int configure_mfc(struct mixer *mixer, int device, char *intf_name, int tag,
                  enum stream_type stype, unsigned int rate,
                  unsigned int channels, unsigned int bits)
{
    int ret = 0;
    uint32_t miid = 0;
    struct apm_module_param_data_t* header = NULL;
    struct param_id_mfc_output_media_fmt_t *mfcConf;
    uint16_t* pcmChannel = NULL;
    uint8_t* payloadInfo = NULL;
    size_t payloadSize = 0, padBytes = 0, size;

    ret = agm_mixer_get_miid(mixer, device, intf_name, stype, tag, &miid);
    if (ret) {
        printf("%s Get MIID from tag data failed\n", __func__);
        return ret;
    }

    payloadSize = sizeof(struct apm_module_param_data_t) +
                  sizeof(struct param_id_mfc_output_media_fmt_t) +
                  sizeof(uint16_t)*channels;

    padBytes = PADDING_8BYTE_ALIGN(payloadSize);

    payloadInfo = (uint8_t*) calloc(1, payloadSize + padBytes);
    if (!payloadInfo) {
        return -ENOMEM;
    }

    header = (struct apm_module_param_data_t*)payloadInfo;
    mfcConf = (struct param_id_mfc_output_media_fmt_t*)(payloadInfo +
               sizeof(struct apm_module_param_data_t));
    pcmChannel = (uint16_t*)(payloadInfo + sizeof(struct apm_module_param_data_t) +
                                       sizeof(struct param_id_mfc_output_media_fmt_t));

    header->module_instance_id = miid;
    header->param_id = PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT;
    header->error_code = 0x0;
    header->param_size = payloadSize - sizeof(struct apm_module_param_data_t);

    mfcConf->sampling_rate = rate;
    mfcConf->bit_width = bits;
    mfcConf->num_channels = channels;
    populateChannelMap(pcmChannel, channels);
    size = payloadSize + padBytes;

    return agm_mixer_set_param(mixer, device, stype, (void *)payloadInfo, (int)size);

}

int set_agm_stream_metadata(struct mixer *mixer, int device, uint32_t val, enum dir d, enum stream_type stype, char *intf_name)
{
    char *stream = "PCM";
    char *control = "metadata";
    char *mixer_str;
    struct mixer_ctl *ctl;
    uint8_t *metadata = NULL;
    struct agm_key_value *gkv = NULL, *ckv = NULL;
    struct prop_data *prop = NULL;
    uint32_t num_gkv = 2, num_ckv = 1, num_props = 0;
    uint32_t gkv_size, ckv_size, prop_size, index = 0;
    int ctl_len = 0, ret = 0, offset = 0;
    char *type = "ZERO";

    if (intf_name)
        type = intf_name;

    ret = set_agm_stream_metadata_type(mixer, device, type, stype);
    if (ret)
        return ret;

    if (stype == STREAM_COMPRESS)
        stream = "COMPRESS";

    if (val == PCM_LL_PLAYBACK || val == COMPRESSED_OFFLOAD_PLAYBACK)
        num_gkv += 1;

    if (val == VOICE_UI) {
        if (intf_name)
            num_gkv = 2;
        else
            num_gkv = 3;
    }

    gkv_size = num_gkv * sizeof(struct agm_key_value);
    ckv_size = num_ckv * sizeof(struct agm_key_value);
    prop_size = sizeof(struct prop_data) + (num_props * sizeof(uint32_t));

    metadata = calloc(1, sizeof(num_gkv) + sizeof(num_ckv) + gkv_size + ckv_size + prop_size);
    if (!metadata)
        return -ENOMEM;

    gkv = calloc(num_gkv, sizeof(struct agm_key_value));
    ckv = calloc(num_ckv, sizeof(struct agm_key_value));
    prop = calloc(1, prop_size);
    if (!gkv || !ckv || !prop) {
        if (ckv)
            free(ckv);
        if (gkv)
            free(gkv);
        free(metadata);
        return -ENOMEM;
    }

    if (val == VOICE_UI) {
        if (intf_name) {
            gkv[index].key = DEVICEPP_TX;
            gkv[index].value = DEVICEPP_TX_FLUENCE_FFECNS;
            index++;
            gkv[index].key = DEVICETX;
            gkv[index].value = HANDSETMIC;
            index++;
        } else {
            gkv[index].key = STREAMTX;
            gkv[index].value = val;
            index++;
            gkv[index].key = INSTANCE;
            gkv[index].value = INSTANCE_1;
            index++;
            gkv[index].key = STREAM_CONFIG;
            gkv[index].value = STREAM_CFG_VUI_SVA;
            index++;
        }
    } else {
        if (d == PLAYBACK)
            gkv[index].key = STREAMRX;
        else
            gkv[index].key = STREAMTX;

        gkv[index].value = val;

        index++;
        if (val == PCM_LL_PLAYBACK || val == COMPRESSED_OFFLOAD_PLAYBACK) {
            gkv[index].key = INSTANCE;
            gkv[index].value = INSTANCE_1;
            index++;
        }

        if (d == PLAYBACK) {
            gkv[index].key = DEVICEPP_RX;
            gkv[index].value = DEVICEPP_RX_AUDIO_MBDRC;
        } else {
            gkv[index].key = DEVICEPP_TX;
            gkv[index].value = DEVICEPP_TX_AUDIO_FLUENCE_SMECNS;
        }
    }

    index = 0;
    ckv[index].key = VOLUME;
    ckv[index].value = LEVEL_2;

    prop->prop_id = 0;  //Update prop_id here
    prop->num_values = num_props;

    memcpy(metadata, &num_gkv, sizeof(num_gkv));
    offset += sizeof(num_gkv);
    memcpy(metadata + offset, gkv, gkv_size);
    offset += gkv_size;
    memcpy(metadata + offset, &num_ckv, sizeof(num_ckv));

    offset += sizeof(num_ckv);
    memcpy(metadata + offset, ckv, ckv_size);
    offset += ckv_size;
    memcpy(metadata + offset, prop, prop_size);

    ctl_len = strlen(stream) + 4 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str) {
        free(metadata);
        return -ENOMEM;
    }
    snprintf(mixer_str, ctl_len, "%s%d %s", stream, device, control);

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(gkv);
        free(ckv);
        free(prop);
        free(metadata);
        free(mixer_str);
        return ENOENT;
    }

    ret = mixer_ctl_set_array(ctl, metadata, sizeof(num_gkv) + sizeof(num_ckv) + gkv_size + ckv_size + prop_size);

    free(gkv);
    free(ckv);
    free(prop);
    free(metadata);
    free(mixer_str);
    return ret;
}

int agm_mixer_register_event(struct mixer *mixer, int device, enum stream_type stype, uint32_t miid, uint8_t is_register)
{
    char *stream = "PCM";
    char *control = "event";
    char *mixer_str;
    struct mixer_ctl *ctl;
    struct agm_event_reg_cfg *event_cfg;
    int payload_size = 0;
    int ctl_len = 0,ret = 0;

    if (stype == STREAM_COMPRESS)
        stream = "COMPRESS";

    ctl_len = strlen(stream) + 4 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str)
        return -ENOMEM;

    snprintf(mixer_str, ctl_len, "%s%d %s", stream, device, control);

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(mixer_str);
        return ENOENT;
    }

    ctl_len = sizeof(struct agm_event_reg_cfg) + payload_size;
    event_cfg = calloc(1, ctl_len);
    if (!event_cfg) {
        free(mixer_str);
        return -ENOMEM;
    }

    event_cfg->module_instance_id = miid;
    event_cfg->event_id = EVENT_ID_DETECTION_ENGINE_GENERIC_INFO;
    event_cfg->event_config_payload_size = payload_size;
    event_cfg->is_register = !!is_register;

    ret = mixer_ctl_set_array(ctl, event_cfg, ctl_len);
    free(event_cfg);
    free(mixer_str);
    return ret;
}

int agm_mixer_get_miid(struct mixer *mixer, int device, char *intf_name,
                       enum stream_type stype, int tag_id, uint32_t *miid)
{
    char *stream = "PCM";
    char *control = "getTaggedInfo";
    char *mixer_str;
    struct mixer_ctl *ctl;
    int ctl_len = 0,ret = 0, i;
    void *payload;
    struct gsl_tag_module_info *tag_info;
    struct gsl_tag_module_info_entry *tag_entry;
    int offset = 0;

    ret = set_agm_stream_metadata_type(mixer, device, intf_name, stype);
    if (ret)
        return ret;

    if (stype == STREAM_COMPRESS)
        stream = "COMPRESS";

    ctl_len = strlen(stream) + 4 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str)
        return -ENOMEM;

    snprintf(mixer_str, ctl_len, "%s%d %s", stream, device, control);

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(mixer_str);
        return ENOENT;
    }

    payload = calloc(1024, sizeof(char));
    if (!payload) {
        free(mixer_str);
        return -ENOMEM;
    }

    ret = mixer_ctl_get_array(ctl, payload, 1024);
    if (ret < 0) {
        printf("Failed to mixer_ctl_get_array\n");
        free(payload);
        free(mixer_str);
        return ret;
    }
    tag_info = payload;
    ret = -1;
    tag_entry = &tag_info->tag_module_entry[0];
    offset = 0;
    for (i = 0; i < tag_info->num_tags; i++) {
        tag_entry += offset/sizeof(struct gsl_tag_module_info_entry);

        offset = sizeof(struct gsl_tag_module_info_entry) + (tag_entry->num_modules * sizeof(struct gsl_module_id_info_entry));
        if (tag_entry->tag_id == tag_id) {
            struct gsl_module_id_info_entry *mod_info_entry;

            if (tag_entry->num_modules) {
                 mod_info_entry = &tag_entry->module_entry[0];
                 *miid = mod_info_entry->module_iid;
                 ret = 0;
                 break;
            }
        }
    }

    free(payload);
    free(mixer_str);
    return ret;
}

int agm_mixer_set_param(struct mixer *mixer, int device,
                        enum stream_type stype, void *payload, int size)
{
    char *stream = "PCM";
    char *control = "setParam";
    char *mixer_str;
    struct mixer_ctl *ctl;
    int ctl_len = 0,ret = 0;

    if (stype == STREAM_COMPRESS)
        stream = "COMPRESS";


    ctl_len = strlen(stream) + 4 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str) {
        free(payload);
        return -ENOMEM;
    }
    snprintf(mixer_str, ctl_len, "%s%d %s", stream, device, control);

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(mixer_str);
        return ENOENT;
    }


    ret = mixer_ctl_set_array(ctl, payload, size);

    free(mixer_str);
    return ret;
}

int agm_mixer_set_param_with_file(struct mixer *mixer, int device,
                                  enum stream_type stype, char *path)
{
    FILE *fp;
    int size, bytes_read;
    void *payload;
    char *stream = "PCM";
    char *control = "setParam";
    char *mixer_str;
    struct mixer_ctl *ctl;
    int ctl_len = 0,ret = 0;

    if (stype == STREAM_COMPRESS)
        stream = "COMPRESS";

    fp = fopen(path, "rb");
    if (!fp) {
        printf("%s: Unable to open file '%s'\n", __func__, path);
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    printf("%s: size of %s file is %d bytes\n", __func__, path, size);

    payload = calloc(1, size);
    if (!payload)
        return -ENOMEM;

    fseek(fp, 0, SEEK_SET);
    bytes_read = fread((char*)payload, 1, size , fp);
    if (bytes_read != size) {
        printf("%s failed to read data from file %s, bytes read = %d\n", __func__, path, bytes_read);
        free(payload);
        return -1;
    }

    ctl_len = strlen(stream) + 4 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str) {
        free(payload);
        fclose(fp);
        return -ENOMEM;
    }
    snprintf(mixer_str, ctl_len, "%s%d %s", stream, device, control);

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(mixer_str);
        free(payload);
        fclose(fp);
        return ENOENT;
    }


    ret = mixer_ctl_set_array(ctl, payload, size);

    free(mixer_str);
    free(payload);
    fclose(fp);
    return ret;
}

int agm_mixer_get_event_param(struct mixer *mixer, int device, enum stream_type stype, uint32_t miid)
{
    char *stream = "PCM";
    char *control = "getParam";
    char *mixer_str;
    struct mixer_ctl *ctl;
    int ctl_len = 0,ret = 0;
    struct apm_module_param_data_t* header;
    struct detection_engine_generic_event_cfg *pEventCfg;
    uint8_t* payload = NULL;
    size_t payloadSize = 0;

    if (stype == STREAM_COMPRESS)
        stream = "COMPRESS";

    ctl_len = strlen(stream) + 4 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str)
        return -ENOMEM;

    snprintf(mixer_str, ctl_len, "%s%d %s", stream, device, control);

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(mixer_str);
        return ENOENT;
    }


    payloadSize = sizeof(struct apm_module_param_data_t) +
                  sizeof(struct detection_engine_generic_event_cfg);

    if (payloadSize % 8 != 0)
        payloadSize = payloadSize + (8 - payloadSize % 8);

    payload = (uint8_t*)malloc((size_t)payloadSize);
    if (!payload) {
        printf("malloc failed\n");
        free(mixer_str);
        return -ENOMEM;
    }
    header = (struct apm_module_param_data_t*)payload;
    header->module_instance_id = miid;
    header->param_id = PARAM_ID_DETECTION_ENGINE_GENERIC_EVENT_CFG;
    header->error_code = 0x0;
    header->param_size = payloadSize -  sizeof(struct apm_module_param_data_t);

    pEventCfg = (struct detection_engine_generic_event_cfg *)
                (payload + sizeof(struct apm_module_param_data_t));

    ret = mixer_ctl_set_array(ctl, payload, payloadSize);
    if (ret) {
         printf("%s set failed\n", __func__);
         goto exit;
    }

    memset(payload, 0, payloadSize);
    ret = mixer_ctl_get_array(ctl, payload, payloadSize);
    if (ret) {
         printf("%s set failed\n", __func__);
         goto exit;
    }

    printf("received payload data is 0x%x\n", pEventCfg->event_mode);
exit:
    free(mixer_str);
    free(payload);
    return ret;
}

int agm_mixer_get_buf_tstamp(struct mixer *mixer, int device, enum stream_type stype, uint64_t *tstamp)
{
    char *stream = "PCM";
    char *control = "bufTimestamp";
    char *mixer_str;
    struct mixer_ctl *ctl;
    int ctl_len = 0,ret = 0;
    uint64_t ts = 0;

    if (stype == STREAM_COMPRESS)
        stream = "COMPRESS";

    ctl_len = strlen(stream) + 4 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str)
        return -ENOMEM;

    snprintf(mixer_str, ctl_len, "%s%d %s", stream, device, control);

    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(mixer_str);
        return ENOENT;
    }

    ret = mixer_ctl_get_array(ctl, &ts, sizeof(uint64_t));
    if (ret) {
         printf("%s get failed\n", __func__);
         goto exit;
    }

    *tstamp = ts;
exit:
    free(mixer_str);
    return ret;
}
