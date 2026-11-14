# 九轴IMU模块V1：驱动接入

公共头文件：[imu9.h](drivers/common/imu9.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
int32_t lsm6ds3tr_c_read_reg(const stmdev_ctx_t *ctx, uint8_t reg, uint8_t *data, uint16_t len);
int32_t lsm6ds3tr_c_write_reg(const stmdev_ctx_t *ctx, uint8_t reg, uint8_t *data, uint16_t len);
float_t lsm6ds3tr_c_from_fs2g_to_mg(int16_t lsb);
float_t lsm6ds3tr_c_from_fs4g_to_mg(int16_t lsb);
float_t lsm6ds3tr_c_from_fs8g_to_mg(int16_t lsb);
float_t lsm6ds3tr_c_from_fs16g_to_mg(int16_t lsb);
float_t lsm6ds3tr_c_from_fs125dps_to_mdps(int16_t lsb);
float_t lsm6ds3tr_c_from_fs250dps_to_mdps(int16_t lsb);
float_t lsm6ds3tr_c_from_fs500dps_to_mdps(int16_t lsb);
float_t lsm6ds3tr_c_from_fs1000dps_to_mdps(int16_t lsb);
float_t lsm6ds3tr_c_from_fs2000dps_to_mdps(int16_t lsb);
float_t lsm6ds3tr_c_from_lsb_to_celsius(int16_t lsb);
int32_t lsm6ds3tr_c_xl_full_scale_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_fs_xl_t val);
int32_t lsm6ds3tr_c_xl_full_scale_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_fs_xl_t *val);
int32_t lsm6ds3tr_c_xl_data_rate_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_odr_xl_t val);
int32_t lsm6ds3tr_c_xl_data_rate_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_odr_xl_t *val);
int32_t lsm6ds3tr_c_gy_full_scale_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_fs_g_t val);
int32_t lsm6ds3tr_c_gy_full_scale_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_fs_g_t *val);
int32_t lsm6ds3tr_c_gy_data_rate_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_odr_g_t val);
int32_t lsm6ds3tr_c_gy_data_rate_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_odr_g_t *val);
int32_t lsm6ds3tr_c_block_data_update_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_block_data_update_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_xl_offset_weight_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_usr_off_w_t val);
int32_t lsm6ds3tr_c_xl_offset_weight_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_usr_off_w_t *val);
int32_t lsm6ds3tr_c_xl_power_mode_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_xl_hm_mode_t val);
int32_t lsm6ds3tr_c_xl_power_mode_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_xl_hm_mode_t *val);
int32_t lsm6ds3tr_c_rounding_on_status_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_rounding_status_t val);
int32_t lsm6ds3tr_c_rounding_on_status_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_rounding_status_t *val);
int32_t lsm6ds3tr_c_gy_power_mode_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_g_hm_mode_t val);
int32_t lsm6ds3tr_c_gy_power_mode_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_g_hm_mode_t *val);
int32_t lsm6ds3tr_c_all_sources_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_all_sources_t *val);
int32_t lsm6ds3tr_c_status_reg_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_status_reg_t *val);
int32_t lsm6ds3tr_c_xl_flag_data_ready_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_gy_flag_data_ready_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_temp_flag_data_ready_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_xl_usr_offset_set(const stmdev_ctx_t *ctx, uint8_t *buff);
int32_t lsm6ds3tr_c_xl_usr_offset_get(const stmdev_ctx_t *ctx, uint8_t *buff);
int32_t lsm6ds3tr_c_timestamp_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_timestamp_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_timestamp_res_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_timer_hr_t val);
int32_t lsm6ds3tr_c_timestamp_res_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_timer_hr_t *val);
int32_t lsm6ds3tr_c_rounding_mode_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_rounding_t val);
int32_t lsm6ds3tr_c_rounding_mode_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_rounding_t *val);
int32_t lsm6ds3tr_c_temperature_raw_get(const stmdev_ctx_t *ctx, int16_t *val);
int32_t lsm6ds3tr_c_angular_rate_raw_get(const stmdev_ctx_t *ctx, int16_t *val);
int32_t lsm6ds3tr_c_acceleration_raw_get(const stmdev_ctx_t *ctx, int16_t *val);
int32_t lsm6ds3tr_c_mag_calibrated_raw_get(const stmdev_ctx_t *ctx, int16_t *val);
int32_t lsm6ds3tr_c_fifo_raw_data_get(const stmdev_ctx_t *ctx, uint8_t *buffer, uint8_t len);
int32_t lsm6ds3tr_c_mem_bank_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_func_cfg_en_t val);
int32_t lsm6ds3tr_c_mem_bank_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_func_cfg_en_t *val);
int32_t lsm6ds3tr_c_data_ready_mode_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_drdy_pulsed_g_t val);
int32_t lsm6ds3tr_c_data_ready_mode_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_drdy_pulsed_g_t *val);
int32_t lsm6ds3tr_c_device_id_get(const stmdev_ctx_t *ctx, uint8_t *buff);
int32_t lsm6ds3tr_c_reset_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_reset_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_data_format_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_ble_t val);
int32_t lsm6ds3tr_c_data_format_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_ble_t *val);
int32_t lsm6ds3tr_c_auto_increment_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_auto_increment_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_boot_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_boot_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_xl_self_test_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_st_xl_t val);
int32_t lsm6ds3tr_c_xl_self_test_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_st_xl_t *val);
int32_t lsm6ds3tr_c_gy_self_test_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_st_g_t val);
int32_t lsm6ds3tr_c_gy_self_test_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_st_g_t *val);
int32_t lsm6ds3tr_c_filter_settling_mask_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_filter_settling_mask_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_xl_hp_path_internal_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_slope_fds_t val);
int32_t lsm6ds3tr_c_xl_hp_path_internal_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_slope_fds_t *val);
int32_t lsm6ds3tr_c_xl_filter_analog_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_bw0_xl_t val);
int32_t lsm6ds3tr_c_xl_filter_analog_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_bw0_xl_t *val);
int32_t lsm6ds3tr_c_xl_lp1_bandwidth_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_lpf1_bw_sel_t val);
int32_t lsm6ds3tr_c_xl_lp1_bandwidth_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_lpf1_bw_sel_t *val);
int32_t lsm6ds3tr_c_xl_lp2_bandwidth_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_input_composite_t val);
int32_t lsm6ds3tr_c_xl_lp2_bandwidth_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_input_composite_t *val);
int32_t lsm6ds3tr_c_xl_reference_mode_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_xl_reference_mode_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_xl_hp_bandwidth_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_hpcf_xl_t val);
int32_t lsm6ds3tr_c_xl_hp_bandwidth_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_hpcf_xl_t *val);
int32_t lsm6ds3tr_c_gy_band_pass_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_lpf1_sel_g_t val);
int32_t lsm6ds3tr_c_gy_band_pass_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_lpf1_sel_g_t *val);
int32_t lsm6ds3tr_c_spi_mode_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_sim_t val);
int32_t lsm6ds3tr_c_spi_mode_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_sim_t *val);
int32_t lsm6ds3tr_c_i2c_interface_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_i2c_disable_t val);
int32_t lsm6ds3tr_c_i2c_interface_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_i2c_disable_t *val);
int32_t lsm6ds3tr_c_pin_int1_route_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_int1_route_t val);
int32_t lsm6ds3tr_c_pin_int1_route_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_int1_route_t *val);
int32_t lsm6ds3tr_c_pin_int2_route_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_int2_route_t val);
int32_t lsm6ds3tr_c_pin_int2_route_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_int2_route_t *val);
int32_t lsm6ds3tr_c_pin_mode_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_pp_od_t val);
int32_t lsm6ds3tr_c_pin_mode_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_pp_od_t *val);
int32_t lsm6ds3tr_c_pin_polarity_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_h_lactive_t val);
int32_t lsm6ds3tr_c_pin_polarity_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_h_lactive_t *val);
int32_t lsm6ds3tr_c_all_on_int1_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_all_on_int1_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_int_notification_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_lir_t val);
int32_t lsm6ds3tr_c_int_notification_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_lir_t *val);
int32_t lsm6ds3tr_c_wkup_threshold_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_wkup_threshold_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_wkup_dur_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_wkup_dur_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_gy_sleep_mode_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_gy_sleep_mode_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_act_mode_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_inact_en_t val);
int32_t lsm6ds3tr_c_act_mode_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_inact_en_t *val);
int32_t lsm6ds3tr_c_act_sleep_dur_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_act_sleep_dur_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_tap_src_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_tap_src_t *val);
int32_t lsm6ds3tr_c_tap_detection_on_z_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_tap_detection_on_z_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_tap_detection_on_y_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_tap_detection_on_y_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_tap_detection_on_x_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_tap_detection_on_x_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_tap_threshold_x_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_tap_threshold_x_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_tap_shock_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_tap_shock_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_tap_quiet_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_tap_quiet_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_tap_dur_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_tap_dur_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_tap_mode_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_single_double_tap_t val);
int32_t lsm6ds3tr_c_tap_mode_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_single_double_tap_t *val);
int32_t lsm6ds3tr_c_6d_feed_data_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_low_pass_on_6d_t val);
int32_t lsm6ds3tr_c_6d_feed_data_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_low_pass_on_6d_t *val);
int32_t lsm6ds3tr_c_6d_threshold_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_sixd_ths_t val);
int32_t lsm6ds3tr_c_6d_threshold_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_sixd_ths_t *val);
int32_t lsm6ds3tr_c_4d_mode_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_4d_mode_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_ff_dur_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_ff_dur_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_ff_threshold_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_ff_ths_t val);
int32_t lsm6ds3tr_c_ff_threshold_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_ff_ths_t *val);
int32_t lsm6ds3tr_c_fifo_watermark_set(const stmdev_ctx_t *ctx, uint16_t val);
int32_t lsm6ds3tr_c_fifo_watermark_get(const stmdev_ctx_t *ctx, uint16_t *val);
int32_t lsm6ds3tr_c_fifo_data_level_get(const stmdev_ctx_t *ctx, uint16_t *val);
int32_t lsm6ds3tr_c_fifo_wtm_flag_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_fifo_pattern_get(const stmdev_ctx_t *ctx, uint16_t *val);
int32_t lsm6ds3tr_c_fifo_temp_batch_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_fifo_temp_batch_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_fifo_write_trigger_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_trigger_fifo_t val);
int32_t lsm6ds3tr_c_fifo_write_trigger_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_trigger_fifo_t *val);
int32_t lsm6ds3tr_c_fifo_pedo_and_timestamp_batch_set( stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_fifo_pedo_and_timestamp_batch_get( stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_fifo_xl_batch_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_dec_fifo_xl_t val);
int32_t lsm6ds3tr_c_fifo_xl_batch_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_dec_fifo_xl_t *val);
int32_t lsm6ds3tr_c_fifo_gy_batch_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_dec_fifo_gyro_t val);
int32_t lsm6ds3tr_c_fifo_gy_batch_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_dec_fifo_gyro_t *val);
int32_t lsm6ds3tr_c_fifo_dataset_3_batch_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_dec_ds3_fifo_t val);
int32_t lsm6ds3tr_c_fifo_dataset_3_batch_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_dec_ds3_fifo_t *val);
int32_t lsm6ds3tr_c_fifo_dataset_4_batch_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_dec_ds4_fifo_t val);
int32_t lsm6ds3tr_c_fifo_dataset_4_batch_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_dec_ds4_fifo_t *val);
int32_t lsm6ds3tr_c_fifo_xl_gy_8bit_format_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_fifo_xl_gy_8bit_format_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_fifo_stop_on_wtm_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_fifo_stop_on_wtm_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_fifo_mode_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_fifo_mode_t val);
int32_t lsm6ds3tr_c_fifo_mode_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_fifo_mode_t *val);
int32_t lsm6ds3tr_c_fifo_data_rate_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_odr_fifo_t val);
int32_t lsm6ds3tr_c_fifo_data_rate_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_odr_fifo_t *val);
int32_t lsm6ds3tr_c_den_polarity_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_den_lh_t val);
int32_t lsm6ds3tr_c_den_polarity_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_den_lh_t *val);
int32_t lsm6ds3tr_c_den_mode_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_den_mode_t val);
int32_t lsm6ds3tr_c_den_mode_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_den_mode_t *val);
int32_t lsm6ds3tr_c_den_enable_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_den_xl_en_t val);
int32_t lsm6ds3tr_c_den_enable_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_den_xl_en_t *val);
int32_t lsm6ds3tr_c_den_mark_axis_z_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_den_mark_axis_z_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_den_mark_axis_y_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_den_mark_axis_y_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_den_mark_axis_x_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_den_mark_axis_x_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_pedo_step_reset_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_pedo_step_reset_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_pedo_sens_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_pedo_sens_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_pedo_threshold_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_pedo_threshold_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_pedo_full_scale_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_pedo_fs_t val);
int32_t lsm6ds3tr_c_pedo_full_scale_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_pedo_fs_t *val);
int32_t lsm6ds3tr_c_pedo_debounce_steps_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_pedo_debounce_steps_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_pedo_timeout_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_pedo_timeout_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_pedo_steps_period_set(const stmdev_ctx_t *ctx, uint8_t *buff);
int32_t lsm6ds3tr_c_pedo_steps_period_get(const stmdev_ctx_t *ctx, uint8_t *buff);
int32_t lsm6ds3tr_c_motion_sens_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_motion_sens_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_motion_threshold_set(const stmdev_ctx_t *ctx, uint8_t *buff);
int32_t lsm6ds3tr_c_motion_threshold_get(const stmdev_ctx_t *ctx, uint8_t *buff);
int32_t lsm6ds3tr_c_tilt_sens_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_tilt_sens_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_wrist_tilt_sens_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_wrist_tilt_sens_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_tilt_latency_set(const stmdev_ctx_t *ctx, uint8_t *buff);
int32_t lsm6ds3tr_c_tilt_latency_get(const stmdev_ctx_t *ctx, uint8_t *buff);
int32_t lsm6ds3tr_c_tilt_threshold_set(const stmdev_ctx_t *ctx, uint8_t *buff);
int32_t lsm6ds3tr_c_tilt_threshold_get(const stmdev_ctx_t *ctx, uint8_t *buff);
int32_t lsm6ds3tr_c_tilt_src_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_a_wrist_tilt_mask_t *val);
int32_t lsm6ds3tr_c_tilt_src_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_a_wrist_tilt_mask_t *val);
int32_t lsm6ds3tr_c_mag_soft_iron_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_mag_soft_iron_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_mag_hard_iron_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_mag_hard_iron_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_mag_soft_iron_mat_set(const stmdev_ctx_t *ctx, uint8_t *buff);
int32_t lsm6ds3tr_c_mag_soft_iron_mat_get(const stmdev_ctx_t *ctx, uint8_t *buff);
int32_t lsm6ds3tr_c_mag_offset_set(const stmdev_ctx_t *ctx, int16_t *val);
int32_t lsm6ds3tr_c_mag_offset_get(const stmdev_ctx_t *ctx, int16_t *val);
int32_t lsm6ds3tr_c_func_en_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_sh_sync_sens_frame_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_sh_sync_sens_frame_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_sh_sync_sens_ratio_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_rr_t val);
int32_t lsm6ds3tr_c_sh_sync_sens_ratio_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_rr_t *val);
int32_t lsm6ds3tr_c_sh_master_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_sh_master_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_sh_pass_through_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_sh_pass_through_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_sh_pin_mode_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_pull_up_en_t val);
int32_t lsm6ds3tr_c_sh_pin_mode_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_pull_up_en_t *val);
int32_t lsm6ds3tr_c_sh_syncro_mode_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_start_config_t val);
int32_t lsm6ds3tr_c_sh_syncro_mode_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_start_config_t *val);
int32_t lsm6ds3tr_c_sh_drdy_on_int1_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_sh_drdy_on_int1_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_sh_read_data_raw_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_emb_sh_read_t *val);
int32_t lsm6ds3tr_c_sh_cmd_sens_sync_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_sh_cmd_sens_sync_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_sh_spi_sync_error_set(const stmdev_ctx_t *ctx, uint8_t val);
int32_t lsm6ds3tr_c_sh_spi_sync_error_get(const stmdev_ctx_t *ctx, uint8_t *val);
int32_t lsm6ds3tr_c_sh_num_of_dev_connected_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_aux_sens_on_t val);
int32_t lsm6ds3tr_c_sh_num_of_dev_connected_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_aux_sens_on_t *val);
int32_t lsm6ds3tr_c_sh_cfg_write(const stmdev_ctx_t *ctx, lsm6ds3tr_c_sh_cfg_write_t *val);
int32_t lsm6ds3tr_c_sh_slv0_cfg_read(const stmdev_ctx_t *ctx, lsm6ds3tr_c_sh_cfg_read_t *val);
int32_t lsm6ds3tr_c_sh_slv1_cfg_read(const stmdev_ctx_t *ctx, lsm6ds3tr_c_sh_cfg_read_t *val);
int32_t lsm6ds3tr_c_sh_slv2_cfg_read(const stmdev_ctx_t *ctx, lsm6ds3tr_c_sh_cfg_read_t *val);
int32_t lsm6ds3tr_c_sh_slv3_cfg_read(const stmdev_ctx_t *ctx, lsm6ds3tr_c_sh_cfg_read_t *val);
int32_t lsm6ds3tr_c_sh_slave_0_dec_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_slave0_rate_t val);
int32_t lsm6ds3tr_c_sh_slave_0_dec_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_slave0_rate_t *val);
int32_t lsm6ds3tr_c_sh_write_mode_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_write_once_t val);
int32_t lsm6ds3tr_c_sh_write_mode_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_write_once_t *val);
int32_t lsm6ds3tr_c_sh_slave_1_dec_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_slave1_rate_t val);
int32_t lsm6ds3tr_c_sh_slave_1_dec_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_slave1_rate_t *val);
int32_t lsm6ds3tr_c_sh_slave_2_dec_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_slave2_rate_t val);
int32_t lsm6ds3tr_c_sh_slave_2_dec_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_slave2_rate_t *val);
int32_t lsm6ds3tr_c_sh_slave_3_dec_set(const stmdev_ctx_t *ctx, lsm6ds3tr_c_slave3_rate_t val);
int32_t lsm6ds3tr_c_sh_slave_3_dec_get(const stmdev_ctx_t *ctx, lsm6ds3tr_c_slave3_rate_t *val);
BspStatus imu9_init(Imu9 *d, const Imu9Config *config);
BspStatus imu9_configure(Imu9 *d, lsm6ds3tr_c_fs_xl_t accel, lsm6ds3tr_c_fs_g_t gyro, lsm6ds3tr_c_odr_xl_t accel_rate, lsm6ds3tr_c_odr_g_t gyro_rate);
BspStatus imu9_read(Imu9 *d, Imu9Sample *out);
BspStatus imu9_events_enable(Imu9 *d);
BspStatus imu9_events_read(Imu9 *d, Imu9Events *out);
BspStatus imu9_steps_reset(Imu9 *d);
BspStatus imu9_fifo_start(Imu9 *d, uint16_t watermark_words);
BspStatus imu9_fifo_status(Imu9 *d, Imu9FifoStatus *out);
BspStatus imu9_fifo_read(Imu9 *d, int16_t *words, size_t capacity, size_t *count, uint16_t *first_pattern);
BspStatus imu9_fifo_stop(Imu9 *d);
BspStatus imu9_mag_configure(Imu9 *d, uint8_t xz_average, uint8_t y_average, bool active_high);
BspStatus imu9_self_test(Imu9 *d, Imu9SelfTest *out);
BspStatus imu9_power_down(Imu9 *d);
void imu9_deinit(Imu9 *d);
```

源文件依赖：`bsp_i2c.h`、`imu9.h`、`limits.h`、`math.h`、`stddef.h`、`stdint.h`、`string.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| I²C 通信速率 | 支持 100 / 400； kHz |
| LSM6DS3TR-C 的 I²C 地址 | 0x6A，7 位地址  |
| IST8310 的 I²C 地址 | 0x0E，7 位地址  |

额外 U3 为中断/复位排针，使用前单独核对；本表只给 I2C 主接口。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
