#include <stdio.h>
#include <iostream>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>
#include <include/SubframeBuffer.h>
#include "include/LTESniffer_Core.h"

// include C-only headers
#ifdef __cplusplus
    extern "C" {
#endif

#include "srsran/common/crash_handler.h"
#include "srsran/common/gen_mch_tables.h"
#include "srsran/phy/io/filesink.h"

#include "include/Sniffer_file_defs.h"
#include <sys/types.h>
#include <sys/stat.h>

#ifdef __cplusplus
}
#undef I // Fix complex.h #define I nastiness when using C++
#endif

#define ENABLE_AGC_DEFAULT
using namespace std;

LTESniffer_Core::LTESniffer_Core(const Args& args):
  go_exit(false),
  args(args),
  state(DECODE_MIB),
  nof_workers(args.nof_sniffer_thread),
  mcs_tracking(args.mcs_tracking_mode, args.target_rnti, args.en_debug, args.sniffer_mode, args.api_mode, est_cfo),
  mcs_tracking_mode(args.mcs_tracking_mode),
  harq_mode(args.harq_mode),
  sniffer_mode(args.sniffer_mode),
  ulsche(args.target_rnti, &ul_harq, args.en_debug),
  api_mode(args.api_mode),
  apiwriter(), // need to declare
  dlwriter(), // need to declare
  dldciwriter(), // need to declare
  ulwriter(), // need to declare
  uldciwriter(), // need to declare
  rarwriter(), // need to declare
  otherwriter(), // need to declare
  filewriter_objs({&apiwriter, &dlwriter, &dldciwriter, &ulwriter, &uldciwriter, &rarwriter, &otherwriter}) // need pointers to persist
{
  /*create pcap writer and name of output file*/    
  auto now = std::chrono::system_clock::now();
  std::time_t cur_time = std::chrono::system_clock::to_time_t(now);
  std::string str_cur_time(std::ctime(&cur_time));
  for(std::string::iterator it = str_cur_time.begin(); it != str_cur_time.end(); ++it) {
    if (*it == ' '){
      *it = '_';
    } else if (*it == ':'){
      *it = '.';
    } else if (*it == '\n'){
      *it = '.';
    }
  }

  std::string stat_folder = "/home/stats/";
  filewriter_objs[FILE_IDX_CONTROL]->open((stat_folder + "LETTUCE_control_" + str_cur_time + "ansi")); 
  std::stringstream control_msg_contruct;

  control_msg_contruct << "\nLTESniffer_Core: Starting...\n\n";
  control_msg_contruct << str_cur_time << std::endl;

  std::string mode_str = "NA";
  if(sniffer_mode==0){mode_str = "DL only";}
  else if(sniffer_mode==1){mode_str = "UL only";}
  else if(sniffer_mode==2){mode_str = "DL & UL";}
  control_msg_contruct << "Sniffer Mode: " << mode_str << std::endl; 

  // File Writers
    // std::string pcap_file_name = "ul_pcap_" + str_cur_time + "pcap";
  std::string pcap_file_name;
  std::string pcap_file_name_api = "api_collector.pcap";
  if (sniffer_mode == DL_MODE){
    pcap_file_name = "ltesniffer_dl_mode.pcap";
  } else {
    pcap_file_name = "ltesniffer_ul_mode.pcap";
  }
  pcapwriter.open(pcap_file_name, pcap_file_name_api, 0);

  struct stat st = {0};
  if (stat(stat_folder.c_str(), &st) == -1) {
      mkdir(stat_folder.c_str(), 0700);
  }

  std::string error_filename = stat_folder + "LETTUCE_stderr_" + str_cur_time + "ansi";
  errfile = freopen(error_filename.c_str(),"w",stderr);

  std::string out_filename = stat_folder + "LETTUCE_stdout_" + str_cur_time + "ansi";
  // this will do the same format on command line with tee 
  //    date +"LETTUCE_stdout_%a_%b__%-d_%H.%M.%S_%Y.ansi"
  //    outfile = freopen(out_filename.c_str(),"w",stdout);

  filewriter_objs[FILE_IDX_API]->open((stat_folder + "LETTUCE_api_" + str_cur_time + "ansi")); 
  filewriter_objs[FILE_IDX_DL]->open((stat_folder + "LETTUCE_dl_tab_" + str_cur_time + "ansi")); 
  filewriter_objs[FILE_IDX_DL_DCI]->open((stat_folder + "LETTUCE_dl_dci_" + str_cur_time + "ansi")); 
  filewriter_objs[FILE_IDX_UL]->open((stat_folder + "LETTUCE_ul_tab_" + str_cur_time + "ansi")); 
  filewriter_objs[FILE_IDX_UL_DCI]->open((stat_folder + "LETTUCE_ul_dci_" + str_cur_time + "ansi")); 
  // filewriter_objs[FILE_IDX_RAR]->open((stat_folder + "LETTUCE_rar_" + str_cur_time + "ansi")); 

  /*Init HARQ*/
  harq.init_HARQ(args.harq_mode);
  /*Set multi offset in ULSchedule*/
  int multi_offset_toggle = 0; 
  if((sniffer_mode == UL_MODE) || (sniffer_mode == DL_UL_MODE)) {
    multi_offset_toggle = 1;
  }
  ulsche.set_multi_offset(multi_offset_toggle); 
  /*Create PHY*/
  phy = new Phy(args.rf_nof_rx_ant,
                args.nof_sniffer_thread,
                args.dci_file_name,
                args.stats_file_name,
                args.skip_secondary_meta_formats,
                args.dci_format_split_ratio,
                args.rnti_histogram_threshold,
                &pcapwriter,
                &filewriter_objs,
                &mcs_tracking,
                &harq,
                args.mcs_tracking_mode,
                args.harq_mode,
                &ulsche);
  phy->getCommon().setShortcutDiscovery(args.enable_shortcut_discovery);
  std::shared_ptr<DCIConsumerList> cons(new DCIConsumerList());
  if(args.dci_file_name != "") {
    cons->addConsumer(static_pointer_cast<SubframeInfoConsumer>(std::shared_ptr<DCIToFile>(new DCIToFile(phy->getCommon().getDCIFile()))));
  } 
  // if(args.enable_ASCII_PRB_plot) {
  //   cons->addConsumer(static_pointer_cast<SubframeInfoConsumer>(std::shared_ptr<DCIDrawASCII>(new DCIDrawASCII())));
  // }
  // if(args.enable_ASCII_power_plot) {
  //   cons->addConsumer(static_pointer_cast<SubframeInfoConsumer>(std::shared_ptr<PowerDrawASCII>(new PowerDrawASCII())));
  // }
  setDCIConsumer(cons);

  /* Init TA buffer to receive samples earlier than DL*/
  ta_buffer.ta_temp_buffer = static_cast<cf_t*>(srsran_vec_malloc(3*static_cast<uint32_t>(sizeof(cf_t))*static_cast<uint32_t>(SRSRAN_SF_LEN_PRB(100))));
  for (int i = 0; i<100; i++){
    ta_buffer.ta_last_sample[i] = 0;
  }

  write_file_and_console(control_msg_contruct.str(), filewriter_objs[FILE_IDX_CONTROL]);
}

bool LTESniffer_Core::run(){
  cell_search_cfg_t cell_detect_config = {.max_frames_pbch    = SRSRAN_DEFAULT_MAX_FRAMES_PBCH,
                                        .max_frames_pss       = SRSRAN_DEFAULT_MAX_FRAMES_PSS,
                                        .nof_valid_pss_frames = SRSRAN_DEFAULT_NOF_VALID_PSS_FRAMES,
                                        .init_agc             = 0,
                                        .force_tdd            = false};
  srsran_cell_t      cell;
  falcon_ue_dl_t     falcon_ue_dl;
  srsran_dl_sf_cfg_t dl_sf;
  srsran_pdsch_cfg_t pdsch_cfg;
  srsran_ue_sync_t   ue_sync;

  std::stringstream control_msg;

#ifndef DISABLE_RF
  srsran_rf_t rf;             // to open RF devices
#endif
  int ret, n;                 // return
  uint8_t mch_table[10];      // unknown
  float search_cell_cfo = 0;  // freg. offset
  uint32_t sfn = 0;           // system frame number
  uint32_t skip_cnt = 0;      // number of skipped subframe
  uint32_t rx_fail_cnt = 0;   // number of failed receive sample errors from UHD
  uint32_t total_sf = 0;
  uint32_t skip_last_1s = 0;
  uint16_t nof_lost_sync = 0;
  int mcs_tracking_timer = 0;
  int update_rnti_timer = 0;
  /* Set CPU affinity*/
  if (args.cpu_affinity > -1) {
    cpu_set_t cpuset;
    pthread_t thread;

    thread = pthread_self();
    for (int i = 0; i < 8; i++) {
      if (((args.cpu_affinity >> i) & 0x01) == 1) {
        printf("Setting pdsch_ue with affinity to core %d\n", i);
        CPU_SET((size_t)i, &cpuset);
      }
      if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset)) {
        ERROR("Error setting main thread affinity to %d", args.cpu_affinity);
        exit(-1);
      }
    }
  }

  /* If RF mode (not file mode)*/
#ifndef DISABLE_RF
  if (args.input_file_name == "") {
    //printf("Opening RF device with %d RX antennas...\n", args.rf_nof_rx_ant);
    control_msg << "Opening RF device with " << args.rf_nof_rx_ant << " RX antennas...\n";
    char rfArgsCStr[1024];
    strncpy(rfArgsCStr, args.rf_args.c_str(), 1024);
    if (srsran_rf_open_multi(&rf, rfArgsCStr, args.rf_nof_rx_ant)) {
      fprintf(stderr, "Error opening rf\n");
      control_msg << "Error opening rf\n";
      exit(-1);
    }
    /* Set receiver gain */
    if (args.rf_gain > 0) {
      srsran_rf_set_rx_gain(&rf, args.rf_gain);
    } else {
      //printf("Starting AGC thread...\n");
      control_msg << "Starting AGC thread...\n";
      if (srsran_rf_start_gain_thread(&rf, false)) {
        ERROR("Error opening rf");
        control_msg << "Error opening rf\n";
        exit(-1);
      }
      srsran_rf_set_rx_gain(&rf, srsran_rf_get_rx_gain(&rf));
      cell_detect_config.init_agc = srsran_rf_get_rx_gain(&rf);
    }

    /* set receiver frequency */
    if (sniffer_mode == UL_MODE && args.ul_freq != 0){
      control_msg << "Tunning DL receiver to " << std::fixed << std::setprecision(3) << ((args.rf_freq + args.file_offset_freq) / 1000000) << std::endl;
      //printf("Tunning DL receiver to %.3f MHz\n", (args.rf_freq + args.file_offset_freq) / 1000000);
      if (srsran_rf_set_rx_freq(&rf, 0, args.rf_freq + args.file_offset_freq)) {
        ///ERROR("Tunning DL Freq failed\n");
      }
      /*Uplink freg*/
      control_msg << "Tunning UL receiver to " << std::fixed << std::setprecision(3) << ((double) (args.ul_freq / 1000000)) << std::endl;
      //printf("Tunning UL receiver to %.3f MHz\n", (double) (args.ul_freq / 1000000));
      if (srsran_rf_set_rx_freq(&rf, 1, args.ul_freq )){
        //ERROR("Tunning UL Freq failed \n");
      }
    } else if (sniffer_mode == UL_MODE && args.ul_freq == 0){
      control_msg << "Uplink Frequency must be defined in the UL Sniffer Mode \n";
      ERROR("Uplink Frequency must be defined in the UL Sniffer Mode");
    } else if (sniffer_mode == DL_MODE && args.ul_freq == 0){
      control_msg << "Tunning DL receiver to " << std::fixed << std::setprecision(3) << ((args.rf_freq + args.file_offset_freq) / 1000000) << std::endl;
      //printf("Tunning receiver to %.3f MHz\n", (args.rf_freq + args.file_offset_freq) / 1000000);
      srsran_rf_set_rx_freq(&rf, args.rf_nof_rx_ant, args.rf_freq + args.file_offset_freq);
    } else if (sniffer_mode == DL_MODE && args.ul_freq != 0){
      control_msg << "Uplink Frequency must be 0 in the DL Sniffer Mode \n";
      ERROR("Uplink Frequency must be 0 in the DL Sniffer Mode");
    } else if (sniffer_mode == DL_UL_MODE && args.ul_freq != 0){ 
      control_msg << "Tunning DL receiver to " << std::fixed << std::setprecision(3) << ((args.rf_freq + args.file_offset_freq) / 1000000) << std::endl;
      //printf("Tunning DL receiver to %.3f MHz\n", (args.rf_freq + args.file_offset_freq) / 1000000);
      if (srsran_rf_set_rx_freq(&rf, 0, args.rf_freq + args.file_offset_freq)) {
        ///ERROR("Tunning DL Freq failed\n");
      }
      /*Uplink freg*/
      control_msg << "Tunning UL receiver to " << std::fixed << std::setprecision(3) << ((double) (args.ul_freq / 1000000)) << std::endl;
      //printf("Tunning UL receiver to %.3f MHz\n", (double) (args.ul_freq / 1000000));
      if (srsran_rf_set_rx_freq(&rf, 1, args.ul_freq )){
        //ERROR("Tunning UL Freq failed \n");
      }
    } else if (sniffer_mode == DL_UL_MODE && args.ul_freq == 0){ 
      control_msg << "Uplink Frequency must be defined in the UL Sniffer Mode \n";
      ERROR("Uplink Frequency must be defined in the UL Sniffer Mode");
    }
    write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
    control_msg.str(std::string());

    if (args.cell_search){
      uint32_t ntrial = 0;
      do {
        control_msg << "Searching for cell...\n";
        ret = rf_search_and_decode_mib(
            &rf, args.rf_nof_rx_ant, &cell_detect_config, args.force_N_id_2, &cell, &search_cell_cfo);
        if (ret < 0) {
          control_msg << "Error searching for cell \n";
          ERROR("Error searching for cell");
          exit(-1);
        } else if (ret == 0 && !go_exit) {
          control_msg << "Cell not found after " << ntrial++ << " trials. Trying again (Press Ctrl+C to exit)\n"; 
          //printf("Cell not found after %d trials. Trying again (Press Ctrl+C to exit)\n", ntrial++);
        }
        write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
        control_msg.str(std::string());
      } while (ret == 0 && !go_exit);
    } else{
      //set up cell manually
      cell.nof_prb          = args.nof_prb;
      cell.id               = args.cell_id;
      cell.nof_ports        = 2;
      cell.cp               = SRSRAN_CP_NORM;
      cell.phich_length     = SRSRAN_PHICH_NORM;
      cell.phich_resources  = SRSRAN_PHICH_R_1_6;
    }
    srsran_rf_stop_rx_stream(&rf);
    // srsran_rf_flush_buffer(&rf);
    if (go_exit) {
      srsran_rf_close(&rf);
      exit(0);
    }

    /* set sampling frequency */
    int srate = srsran_sampling_freq_hz(cell.nof_prb);
    if (srate != -1) {
      control_msg << "Setting sampling rate " << std::fixed << std::setprecision(2) << ((float)srate / 1000000) << " MHz\n";
      //printf("Setting sampling rate %.2f MHz\n", (float)srate / 1000000);
      float srate_rf = srsran_rf_set_rx_srate(&rf, (double)srate);
      if (srate_rf != srate) {
        control_msg << "Could not set sampling rate\n";
        ERROR("Could not set sampling rate");
        exit(-1);
      }
    } else {
      control_msg << "Invalid number of PRB " << cell.nof_prb << std::endl;
      ERROR("Invalid number of PRB %d", cell.nof_prb);
      exit(-1);
    }
    write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
    control_msg.str(std::string());

    INFO("Stopping RF and flushing buffer...\r");
  }
#endif

  /* If reading from file, go straight to PDSCH decoding. Otherwise, decode MIB first */
  if (args.input_file_name != "") {
    /* preset cell configuration */
    cell.id              = args.file_cell_id;
    cell.cp              = SRSRAN_CP_NORM;
    cell.phich_length    = SRSRAN_PHICH_NORM;
    cell.phich_resources = SRSRAN_PHICH_R_1_6;
    cell.nof_ports       = args.file_nof_ports;
    cell.nof_prb         = args.nof_prb;

    char* tmp_filename = new char[args.input_file_name.length()+1];
    strncpy(tmp_filename, args.input_file_name.c_str(), args.input_file_name.length());
    tmp_filename[args.input_file_name.length()] = 0;
    if (srsran_ue_sync_init_file_multi(&ue_sync,
                                       args.nof_prb,
                                       tmp_filename,
                                       args.file_offset_time,
                                       args.file_offset_freq,
                                       args.rf_nof_rx_ant)) { //args.rf_nof_rx_ant
      control_msg << "Error initiating ue_sync" << std::endl;
      ERROR("Error initiating ue_sync");
      write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
      control_msg.str(std::string());
      exit(-1);
    }
    delete[] tmp_filename;
    tmp_filename = nullptr;

  } else {
#ifndef DISABLE_RF
    int decimate = 0;
    if (args.decimate) {
      if (args.decimate > 4 || args.decimate < 0) {
        control_msg << "Invalid decimation factor, setting to 1 \n";
        //printf("Invalid decimation factor, setting to 1 \n");
        write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
        control_msg.str(std::string());
      } else {
        decimate = args.decimate;
      }
    }
    if (srsran_ue_sync_init_multi_decim(&ue_sync,
                                        cell.nof_prb,
                                        cell.id == 1000,
                                        srsran_rf_recv_wrapper,
                                        args.rf_nof_rx_ant,
                                        (void*)&rf,
                                        decimate)) {
      ERROR("Error initiating ue_sync");
      control_msg << "Error initiating ue_sync" << std::endl;
      write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
      control_msg.str(std::string());
      exit(-1);
    }
    if (srsran_ue_sync_set_cell(&ue_sync, cell)) {
      ERROR("Error initiating ue_sync");
      control_msg << "Error initiating ue_sync" << std::endl;
      write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
      control_msg.str(std::string());
      exit(-1);
    }
#endif
  }

  /* set cell for every SubframeWorker*/
  if (!phy->setCell(cell)) {
    cout << "Error initiating UE downlink processing module" << endl;
    return true;
  }

  /*Get 1 worker from available list*/
  std::shared_ptr<SubframeWorker> cur_worker(phy->getAvail());
  cf_t** cur_buffer = cur_worker->getBuffers();

  /* Config mib */
  srsran_ue_mib_t ue_mib;
  if (srsran_ue_mib_init(&ue_mib, cur_buffer[0], cell.nof_prb)) {
    ERROR("Error initaiting UE MIB decoder");
    control_msg << "Error initaiting UE MIB decoder" << std::endl;
    exit(-1);
  }
  if (srsran_ue_mib_set_cell(&ue_mib, cell)) {
    ERROR("Error initaiting UE MIB decoder");
    control_msg << "Error initaiting UE MIB decoder" << std::endl;
    exit(-1);
  }
  write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
  control_msg.str(std::string());

  srsran_sync_set_threshold(&ue_sync.sfind, 2.0);

  // Disable CP based CFO estimation during find
  ue_sync.cfo_current_value       = search_cell_cfo / 15000;
  ue_sync.cfo_is_copied           = true;
  ue_sync.cfo_correct_enable_find = true;
  srsran_sync_set_cfo_cp_enable(&ue_sync.sfind, false, 0);
  
  ZERO_OBJECT(dl_sf);
  ZERO_OBJECT(pdsch_cfg);

  pdsch_cfg.meas_evm_en = true;
  srsran_chest_dl_cfg_t chest_pdsch_cfg = {};
  chest_pdsch_cfg.cfo_estimate_enable   = args.enable_cfo_ref;
  chest_pdsch_cfg.cfo_estimate_sf_mask  = 1023;
  chest_pdsch_cfg.estimator_alg         = srsran_chest_dl_str2estimator_alg(args.estimator_alg.c_str());
  chest_pdsch_cfg.sync_error_enable     = true;

#ifndef DISABLE_RF
  if (args.input_file_name == "") {
    srsran_rf_start_rx_stream(&rf, false);
  }
#endif
#ifndef DISABLE_RF
  if (args.rf_gain < 0 && args.input_file_name == "") {
    srsran_rf_info_t* rf_info = srsran_rf_get_info(&rf);
    srsran_ue_sync_start_agc(&ue_sync,
                             srsran_rf_set_rx_gain_th_wrapper_,
                             rf_info->min_rx_gain,
                             rf_info->max_rx_gain,
                             static_cast<double>(cell_detect_config.init_agc));
  }
#endif

  ue_sync.cfo_correct_enable_track = !args.disable_cfo;
  srsran_pbch_decode_reset(&ue_mib.pbch);

  // Variables for measurements
  uint32_t nframes = 0;
  float    rsrp0 = 0.0, rsrp1 = 0.0, rsrq = 0.0, snr = 0.0, enodebrate = 0.0, uerate = 0.0, procrate = 0.0,
        sinr[SRSRAN_MAX_LAYERS][SRSRAN_MAX_CODEBOOKS] = {}, sync_err[SRSRAN_MAX_PORTS][SRSRAN_MAX_PORTS] = {};
  bool decode_pdsch = false;

  uint64_t sf_cnt          = 0;
  //uint32_t sfn             = 0;
  uint32_t last_decoded_tm = 0;

  /* Length in complex samples */
  uint32_t max_num_samples = 3 * SRSRAN_SF_LEN_PRB(cell.nof_prb); 

  /* Main loop*/
  while (!go_exit && (sf_cnt < args.nof_subframes || args.nof_subframes == 0)){

    /* Set default verbose level */
    set_srsran_verbose_level(args.verbose);
    ret = srsran_ue_sync_zerocopy(&ue_sync, cur_worker->getBuffers(), max_num_samples);
    if (ret < 0) {
      if (args.input_file_name != ""){
        std::cout << "Finish reading from file" << std::endl;
      }
      rx_fail_cnt++;
      ERROR("Error calling srsran_ue_sync_work() cnt: %d", rx_fail_cnt);
      control_msg << "Error calling srsran_ue_sync_work() cnt: " << to_string(rx_fail_cnt) << std::endl;
      if(rx_fail_cnt>UHD_FAIL_LIMIT){
        control_msg << "EXITING because of UHD Error Limit Reached: " << to_string(UHD_FAIL_LIMIT) << std::endl;
        write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
        control_msg.str(std::string());
        return EXIT_FAILURE;
      }
      write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
      control_msg.str(std::string());
    }
    // std:: cout << "CFO = " << srsran_ue_sync_get_cfo(&ue_sync) << std::endl;
#ifdef CORRECT_SAMPLE_OFFSET
    float sample_offset =
        (float)srsran_ue_sync_get_last_sample_offset(&ue_sync) + srsran_ue_sync_get_sfo(&ue_sync) / 1000;
    srsran_ue_dl_set_sample_offset(&ue_dl, sample_offset);
#endif

    if (ret == 1){
      uint32_t sf_idx = srsran_ue_sync_get_sfidx(&ue_sync);
      switch (state) {
        case DECODE_MIB:
          if (sf_idx == 0) {
            uint8_t bch_payload[SRSRAN_BCH_PAYLOAD_LEN];
            int     sfn_offset;
            n = srsran_ue_mib_decode(&ue_mib, bch_payload, NULL, &sfn_offset);
            if (n < 0) {
              ERROR("Error decoding UE MIB");
              control_msg << "Error decoding UE MIB" << std::endl;
              write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
              control_msg.str(std::string());
              exit(-1);
            } else if (n == SRSRAN_UE_MIB_FOUND) {
              srsran_pbch_mib_unpack(bch_payload, &cell, &sfn);
              cell_print(filewriter_objs[FILE_IDX_CONTROL], &cell, sfn);
              //srsran_cell_fprint(stdout, &cell, sfn);
              control_msg << "Decoded MIB. SFN: " << sfn << ", offset: " << sfn_offset << "\n";
              //printf("Decoded MIB. SFN: %d, offset: %d\n", sfn, sfn_offset);
              write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
              control_msg.str(std::string());
              sfn   = (sfn + sfn_offset) % 1024;
              state = DECODE_PDSCH;

              //config RNTI Manager from Falcon Lib
              RNTIManager& rntiManager = phy->getCommon().getRNTIManager();
              // setup rnti manager
              int idx;
              // add format1A evergreens
              idx = falcon_dci_index_of_format_in_list(SRSRAN_DCI_FORMAT1A, falcon_ue_all_formats, nof_falcon_ue_all_formats);
              if(idx > -1) {
                rntiManager.addEvergreen(SRSRAN_RARNTI_START, SRSRAN_RARNTI_END, static_cast<uint32_t>(idx));
                rntiManager.addEvergreen(SRSRAN_PRNTI, SRSRAN_SIRNTI, static_cast<uint32_t>(idx));
              }
              // add format1C evergreens
              idx = falcon_dci_index_of_format_in_list(SRSRAN_DCI_FORMAT1C, falcon_ue_all_formats, nof_falcon_ue_all_formats);
              if(idx > -1) {
                rntiManager.addEvergreen(SRSRAN_RARNTI_START, SRSRAN_RARNTI_END, static_cast<uint32_t>(idx));
                rntiManager.addEvergreen(SRSRAN_PRNTI, SRSRAN_SIRNTI, static_cast<uint32_t>(idx));
              }
              // add forbidden rnti values to rnti manager
              for(uint32_t f=0; f<nof_falcon_ue_all_formats; f++) {
                  //disallow RNTI=0 for all formats
                rntiManager.addForbidden(0x0, 0x0, f);
              }
            }
          }
          break;
        case DECODE_PDSCH:
          if ((mcs_tracking.get_nof_api_msg()%30) == 0 && api_mode > -1){
            print_api_header(filewriter_objs[FILE_IDX_API]);
            //std::cout << std::hash<std::thread::id>{}(std::this_thread::get_id()) << std::endl;
            mcs_tracking.increase_nof_api_msg();
            if (mcs_tracking.get_nof_api_msg() > 30){
              mcs_tracking.reset_nof_api_msg();
            }
          }
          uint32_t tti = sfn * 10 + sf_idx;

          /* Prepare sf_idx and sfn for worker , SF_NRM only*/
          dl_sf.tti = tti;
          dl_sf.sf_type = SRSRAN_SF_NORM;
          cur_worker->prepare(sf_idx, sfn, sf_cnt % (args.dci_format_split_update_interval_ms) == 0, dl_sf);

          /*Get next worker from avail list*/
          std:shared_ptr<SubframeWorker> next_worker;
          if(args.input_file_name == "") {
            next_worker = phy->getAvailImmediate();  //here non-blocking if reading from radio
          } else {
            next_worker = phy->getAvail();  // blocking if reading from file
          }
          if(next_worker != nullptr) {
            phy->putPending(std::move(cur_worker));
            cur_worker = std::move(next_worker);
          } else {
            // cout << "No worker available. Skipping subframe " << sfn << 
            //                                         "." << sf_idx << endl;
            skip_cnt++;
            skip_last_1s++;
          }
          break;
      }

      /*increase system frame number*/
      if (sf_idx == 9) {
        sfn++;
      }
      if (sfn == 1024){
        sfn = 0;
      }
      total_sf++;

      if ((total_sf%1000)==0){// && (api_mode == -1)){
        auto now = std::chrono::system_clock::now();
        std::time_t cur_time = std::chrono::system_clock::to_time_t(now);
        std::string str_cur_time(std::ctime(&cur_time));
        std::string cur_time_second;
        if(str_cur_time.length()>=(11+8)){
          cur_time_second = str_cur_time.substr(11,8);
        }else{
          cur_time_second = "";
        }

        control_msg << "[" << cur_time_second << "] Processed " << (1000 - skip_last_1s) << "/1000 subframes" << "\n";
        control_msg << "Skipped subframe: " << skip_cnt << " / " << sf_cnt << endl;
        control_msg << "Skipped subframes: " << skip_cnt << " (" << static_cast<double>(skip_cnt) * 100 / (phy->getCommon().getStats().nof_subframes + skip_cnt) << "%)" <<  endl;
        write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
        control_msg.str(std::string());

        mcs_tracking_timer++;
        update_rnti_timer ++;
        skip_last_1s = 0;
      }
      if (update_rnti_timer == mcs_tracking.get_interval()){
        //std::cout << "update tables" << std::endl;
        switch (sniffer_mode)
        {
        case DL_MODE:
          if (mcs_tracking_mode && args.target_rnti == 0){ mcs_tracking.update_database_dl(); }
          break;
        case UL_MODE:
          if (mcs_tracking_mode){ mcs_tracking.update_database_ul(); }
          break;
        case DL_UL_MODE: 
          // Downlink
          if (mcs_tracking_mode && args.target_rnti == 0){ mcs_tracking.update_database_dl(); }
          // Uplink
          if (mcs_tracking_mode){ mcs_tracking.update_database_ul(); }
          break;
        default:
          break;
        }
        update_rnti_timer = 0;
      }
      /* Update 256tracking and harq database, delete the inactive RNTIs*/
      if (mcs_tracking_timer == 10){
        //std::cout << "print tables" << std::endl;
        switch (sniffer_mode)
        {
        case DL_MODE:
          mcs_tracking.print_database_dl(filewriter_objs[FILE_IDX_DL], api_mode);
          if (mcs_tracking_mode && args.target_rnti == 0){ mcs_tracking.update_database_dl(); }
          if (harq_mode && args.target_rnti == 0){ harq.updateHARQDatabase(); }
          mcs_tracking_timer = 0;
          break;
        case UL_MODE:
          mcs_tracking.print_database_ul(filewriter_objs[FILE_IDX_UL], api_mode);
          if (mcs_tracking_mode){ mcs_tracking.update_database_ul(); }
          mcs_tracking_timer = 0;
          break;
        case DL_UL_MODE: 
          // Downlink
          mcs_tracking.print_database_dl(filewriter_objs[FILE_IDX_DL], 1); // force DL to not print in DL/UL mode
          if (mcs_tracking_mode && args.target_rnti == 0){ mcs_tracking.update_database_dl(); }
          if (harq_mode && args.target_rnti == 0){ harq.updateHARQDatabase(); }
          // Uplink
          mcs_tracking.print_database_ul(filewriter_objs[FILE_IDX_UL], api_mode);
          if (mcs_tracking_mode){ mcs_tracking.update_database_ul(); }
          mcs_tracking_timer = 0;
          break;
        default:
          break;
        }
      }
    } else if(ret == 0){ //get buffer wrong or out of sync
      //std::cout << "issues" << std::endl;
      /*Change state to Decode MIB to find system frame number again*/
      if (state == DECODE_PDSCH && nof_lost_sync > 5){
        state = DECODE_MIB;
        if (srsran_ue_mib_init(&ue_mib, cur_worker->getBuffers()[0], cell.nof_prb)) {
          ERROR("Error initaiting UE MIB decoder");
          control_msg << "Error initaiting UE MIB decoder" << std::endl;
          exit(-1);
        }
        if (srsran_ue_mib_set_cell(&ue_mib, cell)) {
          control_msg << "Error initaiting UE MIB decoder" << std::endl;
          ERROR("Error initaiting UE MIB decoder");
          exit(-1);
        }
        write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
        control_msg.str(std::string());
        srsran_pbch_decode_reset(&ue_mib.pbch);
        nof_lost_sync = 0;
      }
      
      if(srsran_sync_get_peak_value(&ue_sync.sfind) > srsran_sync_get_threshold(&ue_sync.sfind)){
        control_msg << "Found PSS... NID2: "  << cell.id % 3 <<
                ", Peak: " << srsran_sync_get_peak_value(&ue_sync.sfind) <<
                ", Threshold: " << srsran_sync_get_threshold(&ue_sync.sfind) <<
                ", FrameCnt: " << ue_sync.frame_total_cnt <<
                " State: " << ue_sync.state << endl;
        control_msg << "Missed PSS Attempts: " << nof_lost_sync << endl;
        write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
        control_msg.str(std::string());
      }
      nof_lost_sync++;
      // cout << "Finding PSS... Peak: " << srsran_sync_get_peak_value(&ue_sync.sfind) <<
      //         ", FrameCnt: " << ue_sync.frame_total_cnt <<
      //         " State: " << ue_sync.state << endl;
    }
    sf_cnt++;

  } // main loop

  /* Print statistic of 256tracking*/
  if (mcs_tracking_mode){
    switch (sniffer_mode)
    {
    case DL_MODE:
      mcs_tracking.merge_all_database_dl();
      mcs_tracking.print_all_database_dl(filewriter_objs[FILE_IDX_DL], api_mode); 
      break;
    case UL_MODE:
      mcs_tracking.merge_all_database_ul();
      mcs_tracking.print_all_database_ul(filewriter_objs[FILE_IDX_UL], api_mode); 
      break;
    case DL_UL_MODE: 
      // Downlink
      mcs_tracking.merge_all_database_dl();
      mcs_tracking.print_all_database_dl(filewriter_objs[FILE_IDX_DL], api_mode); // allow final DL table in DL/UL mode
      // Uplink
      mcs_tracking.merge_all_database_ul();
      mcs_tracking.print_all_database_ul(filewriter_objs[FILE_IDX_UL], api_mode); 
      break;
    default:
      break;
    }
  }

  phy->joinPending();

  control_msg << "Destroyed Phy" << std::endl;
  if (args.input_file_name == ""){
    srsran_rf_close(&rf);
    //srsran_ue_dl_free(falcon_ue_dl.q);
    srsran_ue_sync_free(&ue_sync);
    srsran_ue_mib_free(&ue_mib);
  }
  //common->getRNTIManager().printActiveSet();
  control_msg << "Skipped subframe: " << skip_cnt << " / " << sf_cnt << endl;
  //phy->getCommon().getRNTIManager().printActiveSet();
  //rnti_manager_print_active_set(falcon_ue_dl.rnti_manager);

  phy->getCommon().printStats();
  control_msg << "Skipped subframes: " << skip_cnt << " (" << static_cast<double>(skip_cnt) * 100 / (phy->getCommon().getStats().nof_subframes + skip_cnt) << "%)" <<  endl;
  
  write_file_and_console(control_msg.str(), filewriter_objs[FILE_IDX_CONTROL]);
  control_msg.str(std::string());

  /* Print statistic of 256tracking*/
  // if (mcs_tracking_mode){ mcs_tracking.print_database_ul(); }

  /* Print statistic of harq retransmission*/
  //if (harq_mode){ harq.printHARQDatabase(); }
  return EXIT_SUCCESS;
}

void LTESniffer_Core::stop() {
  std::string mystring = "\nLTESniffer_Core: Exiting...\n";
  write_file_and_console(mystring, filewriter_objs[FILE_IDX_CONTROL]);
  go_exit = true;
}

void LTESniffer_Core::handleSignal() {
  stop();
}

LTESniffer_Core::~LTESniffer_Core(){
  pcapwriter.close();
  errfile = freopen("/dev/tty","r",stderr);
  fclose (stderr);
  //outfile = freopen("/dev/tty","r",stdout);
  //fclose (stdout);
  filewriter_objs[FILE_IDX_API]->close(); 
  filewriter_objs[FILE_IDX_DL]->close(); 
  filewriter_objs[FILE_IDX_DL_DCI]->close(); 
  filewriter_objs[FILE_IDX_UL]->close(); 
  filewriter_objs[FILE_IDX_UL_DCI]->close(); 
  //filewriter_objs[FILE_IDX_RAR]->close(); 
  filewriter_objs[FILE_IDX_CONTROL]->close(); 
  // delete        harq_map;
  // harq_map    = nullptr;
  // delete        phy;
  // phy         = nullptr;
  printf("Deleted DL Sniffer core\n");
}

/*function to receive sample from SDR (usrp...)*/
int srsran_rf_recv_wrapper( void* h,
                            cf_t* data_[SRSRAN_MAX_PORTS], 
                            uint32_t nsamples, 
                            srsran_timestamp_t* t){
  DEBUG(" ----  Receive %d samples  ----", nsamples);
  void* ptr[SRSRAN_MAX_PORTS];
  for (int i = 0; i < SRSRAN_MAX_PORTS; i++) {
    ptr[i] = data_[i];
  }
  return srsran_rf_recv_with_time_multi((srsran_rf_t*)h, ptr, nsamples, true, NULL, NULL);
}

void LTESniffer_Core::setDCIConsumer(std::shared_ptr<SubframeInfoConsumer> consumer) {
  phy->getCommon().setDCIConsumer(consumer);
}

void LTESniffer_Core::resetDCIConsumer() {
  phy->getCommon().resetDCIConsumer();
}

RNTIManager& LTESniffer_Core::getRNTIManager(){
  return phy->getCommon().getRNTIManager();
}

void LTESniffer_Core::refreshShortcutDiscovery(bool val){
  phy->getCommon().setShortcutDiscovery(val);
}

void LTESniffer_Core::setRNTIThreshold(int val){
  if(phy){phy->getCommon().getRNTIManager().setHistogramThreshold(val);}
}

void LTESniffer_Core::print_api_header(LTESniffer_stat_writer  *filewriter_obj){
  std::stringstream msg_api;

  for (int i = 0; i < 90; i++){
      msg_api << "-";
  }
  msg_api << std::endl;

  auto now = std::chrono::system_clock::now();
	std::time_t cur_time = std::chrono::system_clock::to_time_t(now);
	std::string str_cur_time(std::ctime(&cur_time));
	std::string cur_time_second;
	if(str_cur_time.length()>=(11+8)){
    cur_time_second = str_cur_time.substr(11,8);
  }else{
    cur_time_second = "";
  }
	msg_api << "[" << cur_time_second << "]: ";

  msg_api << std::left << std::setw(10) <<  "SF";
  msg_api << std::left << std::setw(26) <<  "Detected Identity";
  msg_api << std::left << std::setw(17) <<  "Value";
  msg_api << std::left << std::setw(11) <<  "RNTI";  
  msg_api << std::left << std::setw(25) <<  "From Message";
  msg_api << std::endl;
  for (int i = 0; i < 90; i++){
      msg_api << "-";
  }
  msg_api << std::endl;

  if(DEBUG_SEC_PRINT==1){
		std::cout << msg_api.str();
	}
  if(FILE_WRITE==1){
		filewriter_obj->write_stats(msg_api.str());
	}
}

void write_file_and_console(std::string mystring, LTESniffer_stat_writer* filewriter_obj){
  filewriter_obj->write_stats(mystring);
  std::cout << mystring;
}

void cell_print(LTESniffer_stat_writer* filewriter_obj, srsran_cell_t* cell, uint32_t sfn)
{
  std:string mystring = "";
  mystring = mystring + " - Type:            " + (cell->frame_type == SRSRAN_FDD ? "FDD" : "TDD") + "\n";
  mystring = mystring + " - PCI:             " + std::to_string(cell->id) + "\n";
  mystring = mystring + " - Nof ports:       " + std::to_string(cell->nof_ports) + "\n";
  mystring = mystring + " - CP:              " + (srsran_cp_string(cell->cp)) + "\n";
  mystring = mystring + " - PRB:             " + std::to_string(cell->nof_prb) + "\n";
  mystring = mystring + " - PHICH Length:    " + (cell->phich_length == SRSRAN_PHICH_EXT ? "Extended" : "Normal") + "\n";
  mystring = mystring + " - PHICH Resources: ";
  switch (cell->phich_resources) {
    case SRSRAN_PHICH_R_1_6:
      mystring = mystring + "1/6";
      break;
    case SRSRAN_PHICH_R_1_2:
      mystring = mystring + "1/2";
      break;
    case SRSRAN_PHICH_R_1:
      mystring = mystring + "1";
      break;
    case SRSRAN_PHICH_R_2:
      mystring = mystring + "2";
      break;
  }
  mystring = mystring + "\n";
  mystring = mystring + " - SFN:             " + std::to_string(sfn) + "\n";

  filewriter_obj->write_stats(mystring);
  std::cout << mystring;
}