#include "schrader_bc5a4.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "Schrader"

// https://github.com/merbanan/rtl_433/blob/master/src/devices/schraeder.c
// https://fccid.io/MRXBC5A4

/**
 * Schrader Electronics MRXBC5A4 and MRXBMW433TX1

OEM: BMW 36318532731

* Frequency: 433.92MHz+-38KHz
* Modulation: ASK
* Working Temperature: -50°C to 125°C
* Tire monitoring range value: 0kPa-350kPa+-7kPa

Examples in normal environmental conditions:
fffe088d4980444
ffffc88d499aa44
ffff2235266a910

Data layout:
 * | Byte 0    | Byte 1    | Byte 2    | Byte 3    | Byte 4    | Byte 5    | Byte 6    | Byte 7    |
 * | --------- | --------- | --------- | --------- | --------- | --------- | --------- | --------- |
 * | WSSS SSSS | SSSS SSSF | FFII IIII | IIII IIII | IIII IIII | IIPP PPPP | PPCC TTTT | TTTT

- W: 1 bit wake
- S: 13 sync bit, 1 start bit
- F: 3 bits, might contain status and battery flags.
- I: id (24 bits)
- P: pressure 8 bits 2kPa/bit
- C: Checksum
- T: 8 bits temperature offset by 50, degrees C -50 to 205
*/

#define PREAMBLE 0b11
#define PREAMBLE_BITS_LEN 2

static const SubGhzBlockConst tpms_protocol_schrader_bc5a4_const = {
    .te_short = 123,
    .te_long = 244,
    .te_delta = 62, // 50% of te_short due to poor sensitivity
    .min_count_bit_for_found = 42,
};

struct TPMSProtocolDecoderSchraderBC5A4 {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    ManchesterState manchester_saved_state;
    uint16_t header_count;
};

struct TPMSProtocolEncoderSchraderBC5A4 {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
};

typedef enum {
    SchraderBC5A4DecoderStepReset = 0,
    SchraderBC5A4DecoderStepCheckPreamble,
    SchraderBC5A4DecoderStepDecoderData,
    SchraderBC5A4DecoderStepSaveDuration,
    SchraderBC5A4DecoderStepCheckDuration,
} SchraderBC5A4DecoderStep;

const SubGhzProtocolDecoder tpms_protocol_schrader_bc5a4_decoder = {
    .alloc = tpms_protocol_decoder_schrader_bc5a4_alloc,
    .free = tpms_protocol_decoder_schrader_bc5a4_free,

    .feed = tpms_protocol_decoder_schrader_bc5a4_feed,
    .reset = tpms_protocol_decoder_schrader_bc5a4_reset,

    .get_hash_data = tpms_protocol_decoder_schrader_bc5a4_get_hash_data,
    .serialize = tpms_protocol_decoder_schrader_bc5a4_serialize,
    .deserialize = tpms_protocol_decoder_schrader_bc5a4_deserialize,
    .get_string = tpms_protocol_decoder_schrader_bc5a4_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_schrader_bc5a4_encoder = {
    .alloc = NULL,
    .free = NULL,

    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_schrader_bc5a4 = {
    .name = TPMS_PROTOCOL_SCHRADER_BC5A4_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable,

    .decoder = &tpms_protocol_schrader_bc5a4_decoder,
    .encoder = &tpms_protocol_schrader_bc5a4_encoder,
};

void* tpms_protocol_decoder_schrader_bc5a4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderSchraderBC5A4* instance = malloc(sizeof(TPMSProtocolDecoderSchraderBC5A4));
    instance->base.protocol = &tpms_protocol_schrader_bc5a4;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_schrader_bc5a4_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderBC5A4* instance = context;
    free(instance);
}

void tpms_protocol_decoder_schrader_bc5a4_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderBC5A4* instance = context;
    instance->decoder.parser_step = SchraderBC5A4DecoderStepReset;
}

static bool tpms_protocol_schrader_bc5a4_check_parity(TPMSProtocolDecoderSchraderBC5A4* instance) {
    uint8_t parity = subghz_protocol_blocks_get_parity(instance->decoder.decode_data, instance->decoder.decode_count_bit);
    return parity | 0xFF;
}

/**
 * Analysis of received data
 * @param instance Pointer to a TPMSBlockGeneric* instance
 */
static void tpms_protocol_schrader_bc5a4_analyze(TPMSBlockGeneric* instance) {
    instance->id = instance->data >> (42-24);

    // TODO locate and fix
    instance->battery_low = TPMS_NO_BATT;

    instance->pressure = ((instance->data >> 9) & 0x1FF) / 100;  // Pressure in kpa, convert to bar
    instance->temperature = ((instance->data) & 0xFF) - 50;
}

static ManchesterEvent level_and_duration_to_event(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_schrader_bc5a4_const.te_long) <
       tpms_protocol_schrader_bc5a4_const.te_delta) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_schrader_bc5a4_const.te_short) <
        tpms_protocol_schrader_bc5a4_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }

    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

void tpms_protocol_decoder_schrader_bc5a4_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    bool bit = false;
    bool have_bit = false;
    TPMSProtocolDecoderSchraderBC5A4* instance = context;

    // low-level bit sequence decoding
    if(instance->decoder.parser_step != SchraderBC5A4DecoderStepReset) {
        ManchesterEvent event = level_and_duration_to_event(level, duration);

        if(event == ManchesterEventReset) {
            if((instance->decoder.parser_step == SchraderBC5A4DecoderStepDecoderData) &&
               instance->decoder.decode_count_bit) {
                // FURI_LOG_D(TAG, "%d-%ld", level, duration);
                FURI_LOG_D(
                    TAG,
                    "reset accumulated %d bits: %llx",
                    instance->decoder.decode_count_bit,
                    instance->decoder.decode_data);
            }

            instance->decoder.parser_step = SchraderBC5A4DecoderStepReset;
        } else {
            have_bit = manchester_advance(
                instance->manchester_saved_state, event, &instance->manchester_saved_state, &bit);
            if(!have_bit) return;

            // Invert value, due to signal is Manchester II and decoder is Manchester I
            bit = !bit;
        }
    }

    switch(instance->decoder.parser_step) {
    case SchraderBC5A4DecoderStepReset:
        // wait for start ~480us pulse
        if((level) && (DURATION_DIFF(duration, tpms_protocol_schrader_bc5a4_const.te_long * 2) <
                       tpms_protocol_schrader_bc5a4_const.te_delta)) {
            instance->decoder.parser_step = SchraderBC5A4DecoderStepCheckPreamble;
            instance->header_count = 0;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;

            // First will be short space, so set correct initial state for machine
            // https://clearwater.com.au/images/rc5/rc5-state-machine.gif
            instance->manchester_saved_state = ManchesterStateStart1;
        }
        break;
    case SchraderBC5A4DecoderStepCheckPreamble:
        if(bit != 0) {
            instance->decoder.parser_step = SchraderBC5A4DecoderStepReset;
            break;
        }

        instance->header_count++;
        if(instance->header_count == PREAMBLE_BITS_LEN)
            instance->decoder.parser_step = SchraderBC5A4DecoderStepDecoderData;
        break;

    case SchraderBC5A4DecoderStepDecoderData:
        subghz_protocol_blocks_add_bit(&instance->decoder, bit);
        if(instance->decoder.decode_count_bit >=
           tpms_protocol_schrader_bc5a4_const.min_count_bit_for_found) {
            FURI_LOG_D(TAG, "%016llx", instance->decoder.decode_data);
            if(!tpms_protocol_schrader_bc5a4_check_parity(instance)) {
                FURI_LOG_D(TAG, "CRC mismatch drop");
            } else {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                tpms_protocol_schrader_bc5a4_analyze(&instance->generic);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = SchraderBC5A4DecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_schrader_bc5a4_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderBC5A4* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_schrader_bc5a4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderBC5A4* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_schrader_bc5a4_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderBC5A4* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_schrader_bc5a4_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_schrader_bc5a4_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderBC5A4* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%06lX - %ld\r\n"
        // "Bat:%d\r\n"
        "T:%2.0f°C P:%f kpa/%2.1f psi",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.id,
        // instance->generic.battery_low,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure,
        (double)instance->generic.pressure/(double)6.895
        );
}
