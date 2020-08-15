#include "config.h"
#include <TTGO.h>
#include <soc/rtc.h>
#include "json_psram_allocator.h"

#include "display.h"
#include "pmu.h"
#include "powermgm.h"
#include "motor.h"

#include "gui/statusbar.h"

EventGroupHandle_t pmu_event_handle = NULL;

void IRAM_ATTR pmu_irq( void );

pmu_config_t pmu_config;

/*
 * init the pmu: AXP202 
 */
void pmu_setup( TTGOClass *ttgo ) {
    pmu_event_handle = xEventGroupCreate();

    pmu_read_config();

    // Turn on the IRQ used
    ttgo->power->adc1Enable( AXP202_BATT_VOL_ADC1 | AXP202_BATT_CUR_ADC1 | AXP202_VBUS_VOL_ADC1 | AXP202_VBUS_CUR_ADC1, AXP202_ON);
    ttgo->power->enableIRQ( AXP202_VBUS_REMOVED_IRQ | AXP202_VBUS_CONNECT_IRQ | AXP202_CHARGING_FINISHED_IRQ | AXP202_TIMER_TIMEOUT_IRQ, AXP202_ON );
    ttgo->power->clearIRQ();

    // enable coulumb counter
    if ( ttgo->power->EnableCoulombcounter() ) 
        log_e("enable coulumb counter failed!");    
    if ( ttgo->power->setChargingTargetVoltage( AXP202_TARGET_VOL_4_2V ) )
        log_e("target voltage set failed!");
    if ( ttgo->power->setChargeControlCur( 300 ) )
        log_e("charge current set failed!");
    if ( ttgo->power->setAdcSamplingRate( AXP_ADC_SAMPLING_RATE_25HZ ) )
        log_e("adc sample set failed!");

    // Turn off unused power
    ttgo->power->setPowerOutPut( AXP202_EXTEN, AXP202_OFF );
    ttgo->power->setPowerOutPut( AXP202_DCDC2, AXP202_OFF );
    ttgo->power->setPowerOutPut( AXP202_LDO4, AXP202_OFF );

    // Turn i2s DAC on
    ttgo->power->setLDO3Mode( AXP202_LDO3_MODE_DCIN );
    ttgo->power->setPowerOutPut(AXP202_LDO3, AXP202_ON );

    pinMode( AXP202_INT, INPUT );
    attachInterrupt( AXP202_INT, &pmu_irq, FALLING );
}

/*
 * IRQ routine AXP202
 */
void IRAM_ATTR  pmu_irq( void ) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    /*
     * setup an PMU event
     */
    xEventGroupSetBitsFromISR( pmu_event_handle, PMU_EVENT_AXP_INT, &xHigherPriorityTaskWoken );
    if ( xHigherPriorityTaskWoken ) {
        portYIELD_FROM_ISR();
    }
}

void pmu_standby( void ) {
    TTGOClass *ttgo = TTGOClass::getWatch();

    ttgo->power->clearTimerStatus();
    if ( pmu_get_silence_wakeup() ) {
        if ( ttgo->power->isChargeing() || ttgo->power->isVBUSPlug() ) {
            ttgo->power->setTimer( 3 );
            log_i("enable silence wakeup timer, 3min");
        }
        else {
            ttgo->power->setTimer( 60 );
            log_i("enable silence wakeup timer, 60min");
        }
    }

    if ( pmu_get_experimental_power_save() ) {
        ttgo->power->setDCDC3Voltage( 2700 );
        log_i("go standby, enable 2.7V standby voltage");
    } 
    else {
        ttgo->power->setDCDC3Voltage( 3000 );
        log_i("go standby, enable 3.0V standby voltage");
    }
    ttgo->power->setPowerOutPut(AXP202_LDO3, AXP202_OFF );
}

void pmu_wakeup( void ) {
    TTGOClass *ttgo = TTGOClass::getWatch();

    if ( pmu_get_experimental_power_save() ) {
        ttgo->power->setDCDC3Voltage( 3000 );
        log_i("go wakeup, enable 3.0V voltage");
    } 
    else {
        ttgo->power->setDCDC3Voltage( 3300 );
        log_i("go wakeup, enable 3.3V voltage");
    }

    ttgo->power->clearTimerStatus();
    ttgo->power->offTimer();

    ttgo->power->setPowerOutPut( AXP202_LDO3, AXP202_ON );
}
/*
 *
 */
void pmu_save_config( void ) {
    if ( SPIFFS.exists( PMU_CONFIG_FILE ) ) {
        SPIFFS.remove( PMU_CONFIG_FILE );
        log_i("remove old binary pmu config");
    }
    
    fs::File file = SPIFFS.open( PMU_JSON_CONFIG_FILE, FILE_WRITE );

    if (!file) {
        log_e("Can't open file: %s!", PMU_JSON_CONFIG_FILE );
    }
    else {
        SpiRamJsonDocument doc( 1000 );

        doc["silence_wakeup"] = pmu_config.silence_wakeup;
        doc["experimental_power_save"] = pmu_config.experimental_power_save;
        doc["compute_percent"] = pmu_config.compute_percent;

        if ( serializeJsonPretty( doc, file ) == 0) {
            log_e("Failed to write config file");
        }
        doc.clear();
    }
    file.close();
}

/*
 *
 */
void pmu_read_config( void ) {
    if ( SPIFFS.exists( PMU_JSON_CONFIG_FILE ) ) {        
        fs::File file = SPIFFS.open( PMU_JSON_CONFIG_FILE, FILE_READ );
        if (!file) {
            log_e("Can't open file: %s!", PMU_JSON_CONFIG_FILE );
        }
        else {
            int filesize = file.size();
            SpiRamJsonDocument doc( filesize * 2 );

            DeserializationError error = deserializeJson( doc, file );
            if ( error ) {
                log_e("update check deserializeJson() failed: %s", error.c_str() );
            }
            else {
                pmu_config.silence_wakeup = doc["silence_wakeup"].as<bool>();
                pmu_config.experimental_power_save = doc["experimental_power_save"].as<bool>();
                pmu_config.compute_percent = doc["compute_percent"].as<bool>();
            }        
            doc.clear();
        }
        file.close();
    }
    else {
        log_i("no json config exists, read from binary");
        fs::File file = SPIFFS.open( PMU_CONFIG_FILE, FILE_READ );

        if (!file) {
            log_e("Can't open file: %s!", PMU_CONFIG_FILE );
        }
        else {
            int filesize = file.size();
            if ( filesize > sizeof( pmu_config ) ) {
                log_e("Failed to read configfile. Wrong filesize!" );
            }
            else {
                file.read( (uint8_t *)&pmu_config, filesize );
                file.close();
                pmu_save_config();
                return;                
            }
            file.close();
        }
    }
}

bool pmu_get_silence_wakeup( void ) {
    return( pmu_config.silence_wakeup );
}

void pmu_set_silence_wakeup( bool value ) {
    pmu_config.silence_wakeup = value;
    pmu_save_config();
}

bool pmu_get_calculated_percent( void ) {
    return( pmu_config.compute_percent );
}

bool pmu_get_experimental_power_save( void ) {
    return( pmu_config.experimental_power_save );
}

void pmu_set_calculated_percent( bool value ) {
    pmu_config.compute_percent = value;
    pmu_save_config();
}

void pmu_set_experimental_power_save( bool value ) {
    pmu_config.experimental_power_save = value;
    pmu_save_config();
}

/*
 * loop routine for handling IRQ in main loop
 */
void pmu_loop( TTGOClass *ttgo ) {
    static uint64_t nextmillis = 0;
    bool updatetrigger = false;

    /*
     * handle IRQ event
     */
    if ( xEventGroupGetBitsFromISR( pmu_event_handle ) & PMU_EVENT_AXP_INT ) {        
        ttgo->power->readIRQ();
        if (ttgo->power->isVbusPlugInIRQ()) {
            powermgm_set_event( POWERMGM_WAKEUP_REQUEST );
            motor_vibe( 1 );
            updatetrigger = true;
        }
        if (ttgo->power->isVbusRemoveIRQ()) {
            powermgm_set_event( POWERMGM_WAKEUP_REQUEST );
            motor_vibe( 1 );
            updatetrigger = true;
        }
        if (ttgo->power->isChargingDoneIRQ()) {
            powermgm_set_event( POWERMGM_WAKEUP_REQUEST );
            motor_vibe( 1 );
            updatetrigger = true;
        }
        if (ttgo->power->isPEKShortPressIRQ()) {
            updatetrigger = true;
            powermgm_set_event( POWERMGM_PMU_BUTTON );
            ttgo->power->clearIRQ();
            return;
        }
        if ( ttgo->power->isTimerTimeoutIRQ() ) {
            updatetrigger = true;
            powermgm_set_event( POWERMGM_SILENCE_WAKEUP_REQUEST );
            ttgo->power->clearTimerStatus();
            ttgo->power->offTimer();
            ttgo->power->clearIRQ();
            return;
        }
        ttgo->power->clearIRQ();
        xEventGroupClearBits( pmu_event_handle, PMU_EVENT_AXP_INT );
    }

    if ( !powermgm_get_event( POWERMGM_STANDBY ) ) {
        if ( nextmillis < millis() || updatetrigger == true ) {
            nextmillis = millis() + 1000;
            statusbar_update_battery( pmu_get_battery_percent( ttgo ), ttgo->power->isChargeing(), ttgo->power->isVBUSPlug() );
        }
    }
}

int32_t pmu_get_battery_percent( TTGOClass *ttgo ) {
    if ( ttgo->power->getBattChargeCoulomb() < ttgo->power->getBattDischargeCoulomb() || ttgo->power->getBattVoltage() < 3200 ) {
        ttgo->power->ClearCoulombcounter();
    }

    if ( pmu_get_calculated_percent() ) {
        return( ( ttgo->power->getCoulombData() / PMU_BATTERY_CAP ) * 100 );
    }
    else {
        return( ttgo->power->getBattPercentage() );
    }
}