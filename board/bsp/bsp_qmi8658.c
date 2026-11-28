#include "bsp_qmi8658.h"
#include "bsp_i2c.h"
#include <math.h>

static struct QMI8658Config QMI8658_config;
static unsigned char QMI8658_slave_addr = QMI8658_SLAVE_ADDR_L;

static bool bsp_qmi8658_reg_read(uint8_t reg_addr, uint8_t *data, size_t len)
{
    return bsp_i2c_read_reg8(QMI8658_slave_addr, reg_addr, data, len);
}

static void bsp_qmi8658_reg_write_byte(uint8_t reg_addr, uint8_t *data)
{
    bsp_i2c_write_reg8(QMI8658_slave_addr, reg_addr, data, 1);
}

void bsp_qmi8658_config_acc(enum QMI8658_AccRange range, enum QMI8658_AccOdr odr, enum QMI8658_LpfConfig lpfEnable, enum QMI8658_StConfig stEnable)
{
	unsigned char ctl_dada;

	switch (range)
	{
	case QMI8658AccRange_2g:
	case QMI8658AccRange_4g:
	case QMI8658AccRange_8g:
	case QMI8658AccRange_16g:
		break;
	default:
		range = QMI8658AccRange_8g;
	}
	if (stEnable == QMI8658St_Enable)
		ctl_dada = (unsigned char)range | (unsigned char)odr | 0x80;
	else
		ctl_dada = (unsigned char)range | (unsigned char)odr;

	bsp_qmi8658_reg_write_byte(QMI8658Register_Ctrl2, &ctl_dada);
	// set LPF & HPF
	bsp_qmi8658_reg_read(QMI8658Register_Ctrl5, &ctl_dada, 1);
	ctl_dada &= 0xf0;
	if (lpfEnable == QMI8658Lpf_Enable)
	{
		ctl_dada |= A_LSP_MODE_3;
		ctl_dada |= 0x01;
	}
	else
	{
		ctl_dada &= ~0x01;
	}
	ctl_dada = 0x00;
	bsp_qmi8658_reg_write_byte(QMI8658Register_Ctrl5, &ctl_dada);
	// set LPF & HPF
}

void bsp_qmi8658_config_gyro(enum QMI8658_GyrRange range, enum QMI8658_GyrOdr odr, enum QMI8658_LpfConfig lpfEnable, enum QMI8658_StConfig stEnable)
{
	// Set the CTRL3 register to configure dynamic range and ODR
	unsigned char ctl_dada;

	// Unknown range: 512 dps
	switch (range)
	{
	case QMI8658GyrRange_32dps:
	case QMI8658GyrRange_64dps:
	case QMI8658GyrRange_128dps:
	case QMI8658GyrRange_256dps:
	case QMI8658GyrRange_512dps:
	case QMI8658GyrRange_1024dps:
	case QMI8658GyrRange_2048dps:
	case QMI8658GyrRange_4096dps:
		break;
	default:
		range = QMI8658GyrRange_512dps;
		break;
	}

	if (stEnable == QMI8658St_Enable)
		ctl_dada = (unsigned char)range | (unsigned char)odr | 0x80;
	else
		ctl_dada = (unsigned char)range | (unsigned char)odr;
	bsp_qmi8658_reg_write_byte(QMI8658Register_Ctrl3, &ctl_dada);

	// Conversion from degrees/s to rad/s if necessary
	// set LPF & HPF
	bsp_qmi8658_reg_read(QMI8658Register_Ctrl5, &ctl_dada, 1);
	ctl_dada &= 0x0f;
	if (lpfEnable == QMI8658Lpf_Enable)
	{
		ctl_dada |= G_LSP_MODE_3;
		ctl_dada |= 0x10;
	}
	else
	{
		ctl_dada &= ~0x10;
	}
	ctl_dada = 0x00;
	bsp_qmi8658_reg_write_byte(QMI8658Register_Ctrl5, &ctl_dada);
	// set LPF & HPF
}

float bsp_qmi8658_readTemp(void)
{
	unsigned char buf[2];
	short temp = 0;
	float temp_f = 0;

	if (!bsp_qmi8658_reg_read(QMI8658Register_Tempearture_L, buf, 2))
		return NAN; // opengr8on: failed transfer, buffer not valid
	temp = ((short)buf[1] << 8) | buf[0];
	temp_f = (float)temp / 256.0f;

	return temp_f;
}

void bsp_qmi8658_enableSensors(unsigned char enableFlags)
{
    uint8_t data = enableFlags & QMI8658_CTRL7_ENABLE_MASK;
	bsp_qmi8658_reg_write_byte(QMI8658Register_Ctrl7, &data);
}

void bsp_qmi8658_config_apply(struct QMI8658Config const *config)
{
	unsigned char fisSensors = config->inputSelection;

	// opengr8on: accelerometer / gyroscope only (the AE and magnetometer paths were never used)
	if (config->inputSelection & QMI8658_CONFIG_ACC_ENABLE)
	{
		bsp_qmi8658_config_acc(config->accRange, config->accOdr, QMI8658Lpf_Enable, QMI8658St_Disable);
	}
	if (config->inputSelection & QMI8658_CONFIG_GYR_ENABLE)
	{
		bsp_qmi8658_config_gyro(config->gyrRange, config->gyrOdr, QMI8658Lpf_Enable, QMI8658St_Disable);
	}
	bsp_qmi8658_enableSensors(fisSensors);
}

unsigned char bsp_qmi8658_init(void)
{
	unsigned char QMI8658_chip_id = 0x00;
	unsigned char QMI8658_revision_id = 0x00;
	unsigned char QMI8658_slave[2] = {QMI8658_SLAVE_ADDR_L, QMI8658_SLAVE_ADDR_H};
	unsigned char iCount = 0;
	int retry = 0;

	while (iCount < 2)
	{
		QMI8658_slave_addr = QMI8658_slave[iCount];
		retry = 0;

		while ((QMI8658_chip_id != 0x05) && (retry++ < 5))
		{

			bsp_qmi8658_reg_read(QMI8658Register_WhoAmI, &QMI8658_chip_id, 1);
			QMI8658_printf("QMI8658Register_WhoAmI = 0x%x\n", QMI8658_chip_id);
		}
		if (QMI8658_chip_id == 0x05)
		{
			break;
		}
		iCount++;
	}
	bsp_qmi8658_reg_read(QMI8658Register_Revision, &QMI8658_revision_id, 1);
	if (QMI8658_chip_id == 0x05)
	{
		QMI8658_printf("QMI8658_init slave=0x%x  \r\nQMI8658Register_WhoAmI=0x%x 0x%x\n", QMI8658_slave_addr, QMI8658_chip_id, QMI8658_revision_id);
        uint8_t data = 0x60;
		bsp_qmi8658_reg_write_byte(QMI8658Register_Ctrl1, &data);
		QMI8658_config.inputSelection = QMI8658_CONFIG_ACCGYR_ENABLE; // QMI8658_CONFIG_ACCGYR_ENABLE;
		QMI8658_config.accRange = QMI8658AccRange_8g;
		QMI8658_config.accOdr = QMI8658AccOdr_1000Hz;
		QMI8658_config.gyrRange = QMI8658GyrRange_512dps; // QMI8658GyrRange_2048dps   QMI8658GyrRange_1024dps
		QMI8658_config.gyrOdr = QMI8658GyrOdr_1000Hz;

		bsp_qmi8658_config_apply(&QMI8658_config);
		if (1)
		{
			unsigned char read_data = 0x00;
			bsp_qmi8658_reg_read(QMI8658Register_Ctrl1, &read_data, 1);
			QMI8658_printf("QMI8658Register_Ctrl1=0x%x \n", read_data);
			bsp_qmi8658_reg_read(QMI8658Register_Ctrl2, &read_data, 1);
			QMI8658_printf("QMI8658Register_Ctrl2=0x%x \n", read_data);
			bsp_qmi8658_reg_read(QMI8658Register_Ctrl3, &read_data, 1);
			QMI8658_printf("QMI8658Register_Ctrl3=0x%x \n", read_data);
			bsp_qmi8658_reg_read(QMI8658Register_Ctrl4, &read_data, 1);
			QMI8658_printf("QMI8658Register_Ctrl4=0x%x \n", read_data);
			bsp_qmi8658_reg_read(QMI8658Register_Ctrl5, &read_data, 1);
			QMI8658_printf("QMI8658Register_Ctrl5=0x%x \n", read_data);
			bsp_qmi8658_reg_read(QMI8658Register_Ctrl6, &read_data, 1);
			QMI8658_printf("QMI8658Register_Ctrl6=0x%x \n", read_data);
			bsp_qmi8658_reg_read(QMI8658Register_Ctrl7, &read_data, 1);
			QMI8658_printf("QMI8658Register_Ctrl7=0x%x \n", read_data);
		}
		//		QMI8658_set_layout(2);
		return 1;
	}
	else
	{
		QMI8658_printf("QMI8658_init fail\n");
		QMI8658_chip_id = 0;
		return 0;
	}
	// return QMI8658_chip_id;
}

// ---------------------------------------------------------------------------------------------
// opengr8on: tap detection (QMI8658A/C tap engine). Register sequence as in lewisxhe/SensorLib
// (SensorQMI8658: configTap / enableTap / writeCommand / getTapStatus).
// ---------------------------------------------------------------------------------------------
#define QMI8658_REG_CTRL8      0x09
#define QMI8658_REG_STATUS_INT 0x2d
#define QMI8658_CTRL8_HANDSHAKE_STATUS_INT 0x80 // CTRL9 handshake: STATUSINT bit 7 (not the INT1 pin)
#define QMI8658_CTRL8_ACTIVITY_INT1        0x40 // tap / motion interrupts on INT1
#define QMI8658_CTRL8_TAP_ENABLE           0x01
#define QMI8658_CTRL1_INT1_ENABLE          0x08
#define QMI8658_STATUS_INT_CTRL9_DONE      0x80
#define QMI8658_STATUS1_TAP                0x02
#define QMI8658_CTRL9_CMD_ACK              0x00
#define QMI8658_CTRL9_CMD_CONFIGURE_TAP    0x0c
#define QMI8658_CTRL9_TIMEOUT_US           1500000 // QMI8658A may need ~1.5 s for a CTRL9 command
// Tap timing in accelerometer samples (500 Hz: 2 ms each)
#define QMI8658_TAP_PEAK_WINDOW      20  // 40 ms: the peak must fall within it
#define QMI8658_TAP_WINDOW           50  // 100 ms: quiet after the peak (below the quiet threshold)
// Double tap is not used, but the engine has no switch for it: it reports a single tap only once
// this window ends without a second one. Kept just past QMI8658_TAP_WINDOW (whether the engine
// counts it from the peak or from the end of the quiet window is not documented): +10 ms wake latency.
#define QMI8658_TAP_DOUBLE_WINDOW    (QMI8658_TAP_WINDOW + 5)
#define QMI8658_TAP_PRIORITY_Z_Y_X   0x05
#define QMI8658_TAP_THRESHOLD_SCALE  1024.0f // thresholds: g^2 * 1024 (QST reference: 0.8 * 0x400)

static bool bsp_qmi8658_ctrl9_command(uint8_t command);
static bool bsp_qmi8658_wait_status_int(uint8_t mask, bool set);
static void bsp_qmi8658_write_u16(uint8_t low_reg_addr, uint16_t value);

/**
 * @brief Accelerometer only (8 g, 500 Hz; the gyroscope is not used), tap engine on.
 *
 * @param peak_threshold_g2  squared acceleration a tap must exceed
 * @param quiet_threshold_g2 squared acceleration that must not be exceeded after the peak
 * @return false if the IMU did not complete the configuration (then tap detection stays off)
 */
bool bsp_qmi8658_enable_tap(float peak_threshold_g2, float quiet_threshold_g2)
{
	uint8_t data = QMI8658_CTRL7_DISABLE_ALL;
	bsp_qmi8658_reg_write_byte(QMI8658Register_Ctrl7, &data);
	data = QMI8658_CTRL8_HANDSHAKE_STATUS_INT | QMI8658_CTRL8_ACTIVITY_INT1;
	bsp_qmi8658_reg_write_byte(QMI8658_REG_CTRL8, &data);
	bsp_qmi8658_config_acc(QMI8658AccRange_8g, QMI8658AccOdr_500Hz, QMI8658Lpf_Disable, QMI8658St_Disable);

	data = QMI8658_TAP_PEAK_WINDOW;
	bsp_qmi8658_reg_write_byte(QMI8658Register_Cal1_L, &data);
	data = QMI8658_TAP_PRIORITY_Z_Y_X;
	bsp_qmi8658_reg_write_byte(QMI8658Register_Cal1_H, &data);
	bsp_qmi8658_write_u16(QMI8658Register_Cal2_L, QMI8658_TAP_WINDOW);
	bsp_qmi8658_write_u16(QMI8658Register_Cal3_L, QMI8658_TAP_DOUBLE_WINDOW);
	data = 0x01; // first half of the tap parameters
	bsp_qmi8658_reg_write_byte(QMI8658Register_Cal4_H, &data);
	bool configured = bsp_qmi8658_ctrl9_command(QMI8658_CTRL9_CMD_CONFIGURE_TAP);

	data = (uint8_t)(0.0625f * 128); // alpha: peak detection filter
	bsp_qmi8658_reg_write_byte(QMI8658Register_Cal1_L, &data);
	data = (uint8_t)(0.25f * 128); // gamma: quiet detection filter
	bsp_qmi8658_reg_write_byte(QMI8658Register_Cal1_H, &data);
	bsp_qmi8658_write_u16(QMI8658Register_Cal2_L, (uint16_t)(peak_threshold_g2 * QMI8658_TAP_THRESHOLD_SCALE));
	bsp_qmi8658_write_u16(QMI8658Register_Cal3_L, (uint16_t)(quiet_threshold_g2 * QMI8658_TAP_THRESHOLD_SCALE));
	data = 0x02; // second half of the tap parameters
	bsp_qmi8658_reg_write_byte(QMI8658Register_Cal4_H, &data);
	configured = configured && bsp_qmi8658_ctrl9_command(QMI8658_CTRL9_CMD_CONFIGURE_TAP);

	// opengr8on: CTRL1 is read-modify-write; a failed read must not write the leftover byte over it
	if (bsp_qmi8658_reg_read(QMI8658Register_Ctrl1, &data, 1))
	{
		data |= QMI8658_CTRL1_INT1_ENABLE;
		bsp_qmi8658_reg_write_byte(QMI8658Register_Ctrl1, &data);
	}
	else
	{
		configured = false;
	}
	data = QMI8658_CTRL8_HANDSHAKE_STATUS_INT | QMI8658_CTRL8_ACTIVITY_INT1 | (configured ? QMI8658_CTRL8_TAP_ENABLE : 0);
	bsp_qmi8658_reg_write_byte(QMI8658_REG_CTRL8, &data);
	data = QMI8658_CTRL7_ACC_ENABLE;
	bsp_qmi8658_reg_write_byte(QMI8658Register_Ctrl7, &data);
	return configured;
}

/**
 * @brief Tap since the last call (reading STATUS1 clears it). A double tap counts as one tap.
 * @return false: no tap, or a failed transfer
 */
bool bsp_qmi8658_tap_detected(void)
{
	uint8_t status1 = 0;
	return bsp_qmi8658_reg_read(QMI8658Register_Status1, &status1, 1) && (status1 & QMI8658_STATUS1_TAP);
}

// CTRL9 command: STATUSINT bit 7 signals completion, ACK (0x00) clears it.
static bool bsp_qmi8658_ctrl9_command(uint8_t command)
{
	bsp_qmi8658_reg_write_byte(QMI8658Register_Ctrl9, &command);
	bool done = bsp_qmi8658_wait_status_int(QMI8658_STATUS_INT_CTRL9_DONE, true);
	uint8_t acknowledge = QMI8658_CTRL9_CMD_ACK;
	bsp_qmi8658_reg_write_byte(QMI8658Register_Ctrl9, &acknowledge);
	return bsp_qmi8658_wait_status_int(QMI8658_STATUS_INT_CTRL9_DONE, false) && done;
}

static bool bsp_qmi8658_wait_status_int(uint8_t mask, bool set)
{
	uint64_t deadline_us = time_us_64() + QMI8658_CTRL9_TIMEOUT_US;
	while (time_us_64() < deadline_us)
	{
		uint8_t status_int = 0;
		if (bsp_qmi8658_reg_read(QMI8658_REG_STATUS_INT, &status_int, 1) && ((status_int & mask) != 0) == set)
			return true;
		sleep_ms(1);
	}
	return false;
}

static void bsp_qmi8658_write_u16(uint8_t low_reg_addr, uint16_t value)
{
	uint8_t low_byte = value & 0xff, high_byte = value >> 8;
	bsp_qmi8658_reg_write_byte(low_reg_addr, &low_byte);
	bsp_qmi8658_reg_write_byte(low_reg_addr + 1, &high_byte);
}
