/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under 
 * the Apache License, Version 2.0  (the "License"); you may not use this file
 * except in compliance with the License.  
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*****************************************************************************
  Source      Identification.c

  Version     0.1

  Date        2013/04/09

  Product     NAS stack

  Subsystem   EPS Mobility Management

  Author      Frederic Maurel, Lionel GAUTHIER

  Description Defines the identification EMM procedure executed by the
        Non-Access Stratum.

        The identification procedure is used by the network to request
        a particular UE to provide specific identification parameters
        (IMSI, IMEI).

*****************************************************************************/
#include <pthread.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "bstrlib.h"

#include "dynamic_memory_check.h"
#include "assertions.h"
#include "log.h"
#include "msc.h"
#include "3gpp_requirements_24.301.h"
#include "3gpp_24.007.h"
#include "3gpp_24.008.h"
#include "3gpp_29.274.h"
#include "common_defs.h"
#include "common_types.h"

#include "emm_data.h"
#include "emm_proc.h"
#include "emm_sap.h"
#include "nas_timer.h"
#include "mme_app_ue_context.h"
#include "conversions.h"
#include "mme_app_defs.h"


/****************************************************************************/
/****************  E X T E R N A L    D E F I N I T I O N S  ****************/
/****************************************************************************/

/****************************************************************************/
/*******************  L O C A L    D E F I N I T I O N S  *******************/
/****************************************************************************/

/* String representation of the requested identity type */
static const char                      *_emm_identity_type_str[] = {
  "NOT AVAILABLE", "IMSI", "IMEI", "IMEISV", "TMSI"
};


// callbacks for identification procedure
static void _identification_t3470_handler (void *args);
static int _identification_ll_failure (struct emm_data_context_s *emm_context, struct nas_emm_proc_s * emm_proc);
static int _identification_non_delivered_ho (struct emm_data_context_s *emm_context, struct nas_emm_proc_s * emm_proc);
static int _identification_abort (struct emm_data_context_s *emm_context, struct nas_emm_base_proc_s* emm_base_proc);


static int _identification_request (nas_emm_ident_proc_t * const proc);

/****************************************************************************/
/******************  E X P O R T E D    F U N C T I O N S  ******************/
/****************************************************************************/


/*
   --------------------------------------------------------------------------
        Identification procedure executed by the MME
   --------------------------------------------------------------------------
*/
/********************************************************************
 **                                                                **
 ** Name:    emm_proc_identification()                             **
 **                                                                **
 ** Description: Initiates an identification procedure.            **
 **                                                                **
 **              3GPP TS 24.301, section 5.4.4.2                   **
 **      The network initiates the identification procedure by     **
 **      sending an IDENTITY REQUEST message to the UE and star-   **
 **      ting the timer T3470. The IDENTITY REQUEST message speci- **
 **      fies the requested identification parameters in the Iden- **
 **      tity type information element.                            **
 **                                                                **
 ** Inputs:  ue_id:      UE lower layer identifier                  **
 **      type:      Type of the requested identity                 **
 **      success:   Callback function executed when the identi-    **
 **             fication procedure successfully completes          **
 **      reject:    Callback function executed when the identi-    **
 **             fication procedure fails or is rejected            **
 **      failure:   Callback function executed whener a lower      **
 **             layer failure occured before the identifi-         **
 **             cation procedure completes                         **
 **      Others:    None                                           **
 **                                                                **
 ** Outputs:     None                                              **
 **      Return:    RETURNok, RETURNerror                          **
 **      Others:    _emm_data                                      **
 **                                                                **
 ********************************************************************/
int
emm_proc_identification (
  struct emm_data_context_s     * const emm_context,
  nas_emm_proc_t                * const emm_proc,
  const identity_type2_t           type,
  success_cb_t success,
  failure_cb_t failure)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;

  // todo: why checking the state?
  if ((emm_context) && ((EMM_DEREGISTERED == emm_context->_emm_fsm_state) || (EMM_REGISTERED == emm_context->_emm_fsm_state))){

    REQUIREMENT_3GPP_24_301(R10_5_4_4_1);
    mme_ue_s1ap_id_t                        ue_id = emm_context->ue_id;

    OAILOG_INFO (LOG_NAS_EMM, "EMM-PROC  - Initiate identification type = %s (%d), ctx = %p\n", _emm_identity_type_str[type], type, emm_context);

    /** We may have an identification procedure which was aborted but not freed. */

    nas_emm_ident_proc_t * ident_proc = get_nas_common_procedure_identification(emm_context);
    if (!ident_proc) {
      ident_proc = nas_new_identification_procedure(emm_context);
    }
    if (emm_proc) {
    	/** Maybe triggered due attach, tau request or just implicitly. */
    }
    ident_proc->identity_type                                 = type;
    ident_proc->retransmission_count                          = 0;
    ident_proc->ue_id                                         = ue_id;
    ((nas_emm_base_proc_t *)ident_proc)->parent               = (nas_emm_base_proc_t*)emm_proc;
    ident_proc->emm_com_proc.emm_proc.delivered               = NULL;
    ident_proc->emm_com_proc.emm_proc.previous_emm_fsm_state  = emm_fsm_get_state(emm_context);
    ident_proc->emm_com_proc.emm_proc.not_delivered           = _identification_ll_failure;
    ident_proc->emm_com_proc.emm_proc.not_delivered_ho        = _identification_non_delivered_ho;
    ident_proc->emm_com_proc.emm_proc.base_proc.success_notif = success;
    ident_proc->emm_com_proc.emm_proc.base_proc.failure_notif = failure;
    ident_proc->emm_com_proc.emm_proc.base_proc.abort         = _identification_abort;
    ident_proc->emm_com_proc.emm_proc.base_proc.fail_in       = NULL; // only response
    ident_proc->emm_com_proc.emm_proc.base_proc.time_out      = _identification_t3470_handler;

    rc = _identification_request (ident_proc);

    if (rc != RETURNerror) {
      /*
       * Notify EMM that common procedure has been initiated
       */
      emm_sap_t                               emm_sap = {0};

      emm_sap.primitive = EMMREG_COMMON_PROC_REQ;
      emm_sap.u.emm_reg.ue_id = ue_id;
      emm_sap.u.emm_reg.ctx   = emm_context;
      rc = emm_sap_send (&emm_sap);
      MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_EMM_MME, NULL, 0, "EMMREG_COMMON_PROC_REQ (IDENT) ue id " MME_UE_S1AP_ID_FMT " ",ue_id);
    }
  }

  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

/****************************************************************************
 **                                                                        **
 ** Name:    emm_proc_identification_complete()                            **
 **                                                                        **
 ** Description: Performs the identification completion procedure executed **
 **      by the network.                                                   **
 **                                                                        **
 **              3GPP TS 24.301, section 5.4.4.4                           **
 **      Upon receiving the IDENTITY RESPONSE message, the MME             **
 **      shall stop timer T3470.                                           **
 **                                                                        **
 ** Inputs:  ue_id:      UE lower layer identifier                          **
 **      imsi:      The IMSI received from the UE                          **
 **      imei:      The IMEI received from the UE                          **
 **      tmsi:      The TMSI received from the UE                          **
 **      Others:    None                                                   **
 **                                                                        **
 ** Outputs:     None                                                      **
 **      Return:    RETURNok, RETURNerror                                  **
 **      Others:    _emm_data, T3470                                       **
 **                                                                        **
 ***************************************************************************/
int
emm_proc_identification_complete (
  const mme_ue_s1ap_id_t ue_id,
  imsi_t   * const imsi,
  imei_t   * const imei,
  imeisv_t * const imeisv,
  uint32_t * const tmsi)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;
  emm_sap_t                               emm_sap = {0};
  emm_data_context_t                          *emm_ctx = NULL;

  OAILOG_INFO (LOG_NAS_EMM, "EMM-PROC  - Identification complete (ue_id=" MME_UE_S1AP_ID_FMT ")\n", ue_id);

  // Get the UE context
  emm_ctx = emm_data_context_get(&_emm_data, ue_id);
  if (emm_ctx) {
    nas_emm_ident_proc_t * ident_proc = get_nas_common_procedure_identification(emm_ctx);

    if (ident_proc){
      REQUIREMENT_3GPP_24_301(R10_5_4_4_4);
      /*
       * Stop timer T3470
       */
      void *timer_callback_args = NULL;
      nas_stop_T3470(ue_id, &ident_proc->T3470, timer_callback_args);

      if (imsi) {
        /*
         * Update the IMSI
         */
        imsi64_t imsi64 = imsi_to_imsi64(imsi);

        emm_data_context_t * imsi_emm_ctx_duplicate = emm_data_context_get_by_imsi (&_emm_data, imsi64);
        if(imsi_emm_ctx_duplicate){ /**< We have the UE with this IMSI (different GUTI). */
          if(imsi_emm_ctx_duplicate->ue_id != emm_ctx->ue_id){
			  OAILOG_INFO (LOG_NAS_EMM, "EMM-PROC  - We already have EMM context with ueId " MME_UE_S1AP_ID_FMT " and IMSI " IMSI_64_FMT ". Setting new EMM context with ueId " MME_UE_S1AP_ID_FMT " into pending mode "
				  "and implicitly detaching old EMM context. \n", imsi_emm_ctx_duplicate->ue_id, imsi64, emm_ctx->ue_id);
			  void * unused= NULL;
			  nas_stop_T_retry_specific_procedure(emm_ctx->ue_id, &((nas_emm_specific_proc_t*)(((nas_emm_base_proc_t *)ident_proc)->parent))->retry_timer, unused);
			  nas_start_T_retry_specific_procedure(emm_ctx->ue_id, &((nas_emm_specific_proc_t*)(((nas_emm_base_proc_t *)ident_proc)->parent))->retry_timer, ((nas_emm_specific_proc_t*)(((nas_emm_base_proc_t *)ident_proc)->parent))->retry_cb, emm_ctx);
			  /** Set the old mme_ue_s1ap id which will be checked. */
			  ((nas_emm_specific_proc_t*)(((nas_emm_base_proc_t *)ident_proc)->parent))->old_ue_id = imsi_emm_ctx_duplicate->ue_id;
			  /*
			   * Perform an implicit detach on the new one.
			   */
			  imsi_emm_ctx_duplicate->emm_cause = EMM_CAUSE_ILLEGAL_UE;

			  /** Clean up new UE context that was created to handle new attach request. */
			  memset(&emm_sap, 0 , sizeof(emm_sap_t));
			  emm_sap.primitive = EMMCN_IMPLICIT_DETACH_UE; /**< UE context will be purged. */
			  emm_sap.u.emm_cn.u.emm_cn_implicit_detach.emm_cause   = imsi_emm_ctx_duplicate->emm_cause; /**< Not sending detach type. */
			  emm_sap.u.emm_cn.u.emm_cn_implicit_detach.detach_type = 0; /**< Not sending detach type. */
			  emm_sap.u.emm_cn.u.emm_cn_implicit_detach.ue_id = imsi_emm_ctx_duplicate->ue_id;
			  /*
			   * Don't send the detach type, such that no NAS Detach Request is sent to the UE.
			   * Depending on the cause, the MME_APP will check and inform the NAS layer to continue with the procedure, before the timer expires.
			   */
			  emm_sap_send (&emm_sap);

			  /*
			   * Notify EMM that the identification procedure successfully completed.
			   * Free the procedure, but don't continue.
			   */
			  memset(&emm_sap, 0 , sizeof(emm_sap_t));
			  MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_EMM_MME, NULL, 0, "EMMREG_COMMON_PROC_CNF (IDENT) ue id " MME_UE_S1AP_ID_FMT " ", ue_id);
			  emm_sap.primitive = EMMREG_COMMON_PROC_CNF;
			  emm_sap.u.emm_reg.ue_id    = ue_id;
			  emm_sap.u.emm_reg.ctx      = emm_ctx;
			  emm_sap.u.emm_reg.notify   = false;
			  emm_sap.u.emm_reg.free_proc = true;
			  emm_sap.u.emm_reg.u.common.common_proc            = &ident_proc->emm_com_proc;
			  emm_sap.u.emm_reg.u.common.previous_emm_fsm_state = ident_proc->emm_com_proc.emm_proc.previous_emm_fsm_state;
			  rc = emm_sap_send (&emm_sap);

			  //              unlock_ue_contexts(ue_context);
			  //             unlock_ue_contexts(imsi_ue_mm_ctx);
			  OAILOG_FUNC_RETURN (LOG_NAS_EMM, RETURNok);
          }
        }
        ue_context_t * ue_context_duplicate_imsi = mme_ue_context_exists_imsi(&mme_app_desc.mme_ue_contexts, imsi64);
        if(ue_context_duplicate_imsi &&
        		(ue_context_duplicate_imsi->mme_ue_s1ap_id != emm_ctx->ue_id)){
          OAILOG_ERROR(LOG_NAS_EMM, "EMM-PROC  - We already have MME_APP UE context with ueId " MME_UE_S1AP_ID_FMT " and IMSI " IMSI_64_FMT ". "
              "Setting new EMM context with ueId " MME_UE_S1AP_ID_FMT " into pending mode "
              "and implicitly removing old MME_APP UE context. \n", ue_context_duplicate_imsi->mme_ue_s1ap_id, imsi64, emm_ctx->ue_id);

          nas_itti_esm_detach_ind(ue_context_duplicate_imsi->mme_ue_s1ap_id, false);

          void * unused= NULL;
          nas_stop_T_retry_specific_procedure(emm_ctx->ue_id, &((nas_emm_specific_proc_t*)(((nas_emm_base_proc_t *)ident_proc)->parent))->retry_timer, unused);
          nas_start_T_retry_specific_procedure(emm_ctx->ue_id, &((nas_emm_specific_proc_t*)(((nas_emm_base_proc_t *)ident_proc)->parent))->retry_timer, ((nas_emm_specific_proc_t*)(((nas_emm_base_proc_t *)ident_proc)->parent))->retry_cb, emm_ctx);
          /** Set the old mme_ue_s1ap id which will be checked. */
          ((nas_emm_specific_proc_t*)(((nas_emm_base_proc_t *)ident_proc)->parent))->old_ue_id = ue_context_duplicate_imsi->mme_ue_s1ap_id;

          /*
           * Notify EMM that the identification procedure successfully completed.
           * Free the procedure, but don't continue.
           */
          MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_EMM_MME, NULL, 0, "EMMREG_COMMON_PROC_CNF (IDENT) ue id " MME_UE_S1AP_ID_FMT " ", ue_id);
          emm_sap.primitive = EMMREG_COMMON_PROC_CNF;
          emm_sap.u.emm_reg.ue_id    = ue_id;
          emm_sap.u.emm_reg.ctx      = emm_ctx;
          emm_sap.u.emm_reg.notify   = false;
          emm_sap.u.emm_reg.free_proc = true;
          emm_sap.u.emm_reg.u.common.common_proc            = &ident_proc->emm_com_proc;
          emm_sap.u.emm_reg.u.common.previous_emm_fsm_state = ident_proc->emm_com_proc.emm_proc.previous_emm_fsm_state;
          rc = emm_sap_send (&emm_sap);

          OAILOG_FUNC_RETURN (LOG_NAS_EMM, RETURNok);
        }

        emm_ctx_set_valid_imsi(emm_ctx, imsi, imsi64);
        emm_data_context_upsert_imsi(&_emm_data, emm_ctx);

        emm_data_context_t * imsi_emm_ctx_test = emm_data_context_get_by_imsi (&_emm_data, imsi64);
        DevAssert(imsi_emm_ctx_test);

      } else if (imei) {
        /*
         * Update the IMEI
         */
        emm_ctx_set_valid_imei(emm_ctx, imei);
      } else if (imeisv) {
        /*
         * Update the IMEISV
         */
        emm_ctx_set_valid_imeisv(emm_ctx, imeisv);
      } else if (tmsi) {
        /*
         * Update the GUTI
         */
        AssertFatal(false, "TODO, should not happen because this type of identity is not requested by MME");
      }

      /*
       * Notify EMM that the identification procedure successfully completed
       */
      MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_EMM_MME, NULL, 0, "EMMREG_COMMON_PROC_CNF (IDENT) ue id " MME_UE_S1AP_ID_FMT " ", ue_id);
      emm_sap.primitive = EMMREG_COMMON_PROC_CNF;
      emm_sap.u.emm_reg.ue_id    = ue_id;
      emm_sap.u.emm_reg.ctx      = emm_ctx;
      emm_sap.u.emm_reg.notify   = true;
      emm_sap.u.emm_reg.free_proc = true;
      emm_sap.u.emm_reg.u.common.common_proc            = &ident_proc->emm_com_proc;
      emm_sap.u.emm_reg.u.common.previous_emm_fsm_state = ident_proc->emm_com_proc.emm_proc.previous_emm_fsm_state;
      rc = emm_sap_send (&emm_sap);

    }// else ignore the response if procedure not found
  } // else ignore the response if ue context not found

  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}


/****************************************************************************/
/*********************  L O C A L    F U N C T I O N S  *********************/
/****************************************************************************/

/*
   --------------------------------------------------------------------------
                Timer handlers
   --------------------------------------------------------------------------
*/

/****************************************************************************
 **                                                                        **
 ** Name:    _identification_t3470_handler()                           **
 **                                                                        **
 ** Description: T3470 timeout handler                                     **
 **      Upon T3470 timer expiration, the identification request   **
 **      message is retransmitted and the timer restarted. When    **
 **      retransmission counter is exceed, the MME shall abort the **
 **      identification procedure and any ongoing EMM procedure.   **
 **                                                                        **
 **              3GPP TS 24.301, section 5.4.4.6, case b                   **
 **                                                                        **
 ** Inputs:  args:      handler parameters                         **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     None                                                      **
 **      Return:    None                                       **
 **      Others:    None                                       **
 **                                                                        **
 ***************************************************************************/
static void _identification_t3470_handler (void *args)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  emm_data_context_t                       *emm_ctx = (emm_data_context_t *) (args);

  if (!(emm_ctx)) {
    OAILOG_ERROR (LOG_NAS_EMM, "T3470 timer expired No EMM context\n");
    OAILOG_FUNC_OUT (LOG_NAS_EMM);
  }
  nas_emm_ident_proc_t * ident_proc = get_nas_common_procedure_identification(emm_ctx);

  if (ident_proc){
    OAILOG_WARNING (LOG_NAS_EMM, "T3470 timer (%lx) expired ue id " MME_UE_S1AP_ID_FMT " \n",
        ident_proc->T3470.id, ident_proc->ue_id);
    ident_proc->T3470.id = NAS_TIMER_INACTIVE_ID;
   /*
     * Increment the retransmission counter
     */
    ident_proc->retransmission_count += 1;
    OAILOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - T3470 (%lx) retransmission counter = %d ue id " MME_UE_S1AP_ID_FMT " \n",
        ident_proc->T3470.id, ident_proc->retransmission_count, ident_proc->ue_id);

    if (ident_proc->retransmission_count < IDENTIFICATION_COUNTER_MAX) {
      REQUIREMENT_3GPP_24_301(R10_5_4_4_6_b__1);
      /*
       * Send identity request message to the UE
       */
      _identification_request (ident_proc);
    } else {

      /*
      * Abort the identification procedure
      */
      REQUIREMENT_3GPP_24_301(R10_5_4_4_6_b__2);
      emm_sap_t                               emm_sap = {0};
      emm_sap.primitive = EMMREG_COMMON_PROC_ABORT;
      emm_sap.u.emm_reg.ue_id = ident_proc->ue_id;
      emm_sap.u.emm_reg.ctx   = emm_ctx;
      emm_sap.u.emm_reg.notify= false;
      emm_sap.u.emm_reg.free_proc = true;
      emm_sap.u.emm_reg.u.common.common_proc   = (nas_emm_common_proc_t*)(&ident_proc->emm_com_proc);
      emm_sap.u.emm_reg.u.common.previous_emm_fsm_state   =  ((nas_emm_proc_t*)ident_proc)->previous_emm_fsm_state;
      MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_EMM_MME, NULL, 0, "0 EMMREG_PROC_ABORT (identification) ue id " MME_UE_S1AP_ID_FMT " ", ident_proc->ue_id);
      emm_sap_send (&emm_sap);
      nas_delete_all_emm_procedures(emm_ctx);

      /*
       * We need also to check if the EMM context exists or not.
       * A non delivery indicator, might have triggered another identity request, and in the meantime, the MME_APP context might have been removed, due to a Context Release Complete.
       */

      emm_ctx = emm_data_context_get(&_emm_data, ident_proc->ue_id);
      if(emm_ctx){
        OAILOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - EMM Context for ueId " MME_UE_S1AP_ID_FMT " is still existing. Removing failed EMM context.. \n", ident_proc->ue_id);
        emm_sap_t                               emm_sap = {0};
        emm_sap.primitive = EMMCN_IMPLICIT_DETACH_UE;
        emm_sap.u.emm_cn.u.emm_cn_implicit_detach.ue_id = ident_proc->ue_id;
        emm_sap_send (&emm_sap);
        OAILOG_FUNC_OUT (LOG_NAS_EMM);
      }else{
        OAILOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - EMM Context for ueId " MME_UE_S1AP_ID_FMT " is not existing. Triggering an MME_APP detach.. \n", ident_proc->ue_id);
        nas_itti_esm_detach_ind(ident_proc->ue_id, false);
        OAILOG_FUNC_OUT (LOG_NAS_EMM);
      }
    }
  } else {
    OAILOG_ERROR (LOG_NAS_EMM, "T3470 timer expired, No Identification procedure found\n");
  }

  OAILOG_FUNC_OUT (LOG_NAS_EMM);
}

/*
   --------------------------------------------------------------------------
                MME specific local functions
   --------------------------------------------------------------------------
*/

/*
 * Description: Sends IDENTITY REQUEST message and start timer T3470.
 *
 * Inputs:  args:      handler parameters
 *      Others:    None
 *
 * Outputs:     None
 *      Return:    None
 *      Others:    T3470
 */
static int _identification_request (nas_emm_ident_proc_t * const proc)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  emm_sap_t                          emm_sap = {0};
  int                                rc      = RETURNok;
  struct emm_data_context_s              *emm_ctx = NULL;

  emm_ctx = emm_data_context_get(&_emm_data, proc->ue_id);
  if (!emm_ctx) {
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, RETURNerror);
  }
  /*
   * Notify EMM-AS SAP that Identity Request message has to be sent
   * to the UE
   */
  MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_EMM_MME, NULL, 0, "EMMAS_SECURITY_REQ ue id " MME_UE_S1AP_ID_FMT " IDENTIFICATION", proc->ue_id);
  emm_sap.primitive = EMMAS_SECURITY_REQ;
  emm_sap.u.emm_as.u.security.puid = proc->emm_com_proc.emm_proc.base_proc.nas_puid;
  emm_sap.u.emm_as.u.security.guti = NULL;
  emm_sap.u.emm_as.u.security.ue_id = proc->ue_id;
  emm_sap.u.emm_as.u.security.msg_type = EMM_AS_MSG_TYPE_IDENT;
  emm_sap.u.emm_as.u.security.ident_type = proc->identity_type;


  /*
   * Setup EPS NAS security data
   */
  emm_as_set_security_data (&emm_sap.u.emm_as.u.security.sctx, &emm_ctx->_security, false, true);
  MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_EMM_MME, NULL, 0, "0 EMMAS_SECURITY_REQ (identification) ue id " MME_UE_S1AP_ID_FMT " ", proc->ue_id);
  rc = emm_sap_send (&emm_sap);

  if (rc != RETURNerror) {
    REQUIREMENT_3GPP_24_301(R10_5_4_4_2);
    /*
     * Start T3470 timer
     */
    nas_start_T3470(proc->ue_id, &proc->T3470, proc->emm_com_proc.emm_proc.base_proc.time_out, (void*)emm_ctx);
  }

//  unlock_ue_contexts(ue_context);
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}


//------------------------------------------------------------------------------
static int _identification_ll_failure (struct emm_data_context_s *emm_context, struct nas_emm_proc_s * emm_proc)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;
  if ((emm_context) && (emm_proc)) {
    REQUIREMENT_3GPP_24_301(R10_5_4_4_6_a);
    nas_delete_all_emm_procedures(emm_context);
  }
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}


//------------------------------------------------------------------------------
static int _identification_non_delivered_ho (struct emm_data_context_s *emm_context, struct nas_emm_proc_s * emm_proc)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;
  if ((emm_context) && (emm_proc)) {
    REQUIREMENT_3GPP_24_301(R10_5_4_2_7_j);
    rc = _identification_request((nas_emm_ident_proc_t *)emm_proc);
  }
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}


//------------------------------------------------------------------------------
/*
 * Description: Aborts the identification procedure currently in progress
 *
 * Inputs:  args:      Identification data to be released
 *      Others:    None
 *
 * Outputs:     None
 *      Return:    None
 *      Others:    T3470
 */
static int _identification_abort (struct emm_data_context_s *emm_context, struct nas_emm_base_proc_s* emm_base_proc)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;

  if ((emm_context) && (emm_base_proc)) {
    nas_emm_ident_proc_t * ident_proc = (nas_emm_ident_proc_t*)emm_base_proc;
    AssertFatal((NAS_EMM_PROC_TYPE_COMMON == ((nas_emm_proc_t*)emm_base_proc)->type) &&
        (EMM_COMM_PROC_IDENT == ((nas_emm_common_proc_t*)emm_base_proc)->type), "Mismatch in procedure type");

    OAILOG_INFO (LOG_NAS_EMM, "EMM-PROC  - Abort identification procedure " "(ue_id=" MME_UE_S1AP_ID_FMT ")\n", ident_proc->ue_id);

    /*
     * Stop timer T3470
     */
    void * callback_arg = NULL;
    nas_stop_T3470(ident_proc->ue_id, &ident_proc->T3470, callback_arg);
  }
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}
