#include "m_common_data.h"
#include "m_card.h"
#include <stdio.h>
#include <stddef.h>

#define P(t) printf("%-44s %8zu  0x%zX\n", #t, sizeof(t), sizeof(t))
#define O(s,f) printf("  %-40s off=%8zu 0x%06zX size=%8zu\n", #f, offsetof(s,f), offsetof(s,f), sizeof(((s*)0)->f))

int main(void) {
    printf("== top level ==\n");
    P(Save_t); P(Save); P(common_data_t);
    P(mCD_others_c); P(mCD_keep_original_c); P(mCD_keep_mail_c); P(mCD_keep_diary_c);
    P(MemcardHeader_c); P(PresentSave_c); P(PresentSaveFile_c);
    printf("OTHERS_SIZE            = %8u 0x%X\n", (unsigned)OTHERS_SIZE, (unsigned)OTHERS_SIZE);
    printf("mCD_LAND_SAVE_SIZE     = %8u 0x%X\n", mCD_LAND_SAVE_SIZE, mCD_LAND_SAVE_SIZE);
    printf("mCD_MEMCARD_SECTORSIZE = %8u 0x%X\n", mCD_MEMCARD_SECTORSIZE, mCD_MEMCARD_SECTORSIZE);
    printf("PLAYER_NUM=%d ANIMAL_NUM_MAX=%d\n", PLAYER_NUM, ANIMAL_NUM_MAX);
    printf("FG_BLOCK_X_NUM=%d FG_BLOCK_Z_NUM=%d BLOCK_X_NUM=%d BLOCK_Z_NUM=%d UT_Z_NUM=%d\n",
           FG_BLOCK_X_NUM, FG_BLOCK_Z_NUM, BLOCK_X_NUM, BLOCK_Z_NUM, UT_Z_NUM);
    printf("mNtc_BOARD_POST_COUNT=%d mFR_RECORD_NUM=%d\n", mNtc_BOARD_POST_COUNT, mFR_RECORD_NUM);

    printf("\n== Save_t members (in declaration order) ==\n");
    O(Save_t, save_check);
    O(Save_t, scene_no);
    O(Save_t, private_data);
    O(Save_t, land_info);
    O(Save_t, noticeboard);
    O(Save_t, homes);
    O(Save_t, fg);
    O(Save_t, combi_table);
    O(Save_t, animals);
    O(Save_t, last_removed_animal_id);
    O(Save_t, shop);
    O(Save_t, kabu_price_schedule);
    O(Save_t, event_save_data);
    O(Save_t, event_save_common);
    O(Save_t, post_office);
    O(Save_t, police_box);
    O(Save_t, snowmen);
    O(Save_t, config);
    O(Save_t, deposit);
    O(Save_t, mother_mail);
    O(Save_t, npc_used_tbl);
    O(Save_t, museum_display);
    O(Save_t, bridge);
    O(Save_t, needlework);
    O(Save_t, time_delta);
    O(Save_t, island);
    O(Save_t, allgrow_ss_pos_info);
    O(Save_t, fishRecord);
    O(Save_t, mask_cat);
    O(Save_t, return_animal);
    O(Save_t, LightHouse);
    O(Save_t, good_field);

    printf("\n== sub-struct sizes ==\n");
    P(Private_c); P(mHm_hs_c); P(Animal_c); P(Mail_c); P(Island_c);
    P(mNW_needlework_c); P(MaskCat_c); P(mMmd_info_c); P(Shop_c);
    P(PostOffice_c); P(mFM_fg_c); P(mFM_combination_c); P(mNtc_board_post_c);
    P(mDi_entry_c); P(mNW_original_design_c); P(mFR_record_c); P(PoliceBox_c);
    P(mEv_event_save_c); P(mEv_save_common_data_c); P(mHm_cottage_c);

    printf("\n== Private_c members ==\n");
    O(Private_c, player_ID);
    O(Private_c, museum_record);
    O(Private_c, inventory);
    O(Private_c, deliveries);
    O(Private_c, errands);
    O(Private_c, saved_mail_header);
    O(Private_c, mail);
    O(Private_c, cloth);
    O(Private_c, catalog_orders);
    O(Private_c, furniture_collected_bitfield);
    O(Private_c, wall_collected_bitfield);
    O(Private_c, carpet_collected_bitfield);
    O(Private_c, paper_collected_bitfield);
    O(Private_c, music_collected_bitfield);
    O(Private_c, maps);
    O(Private_c, my_org);
    O(Private_c, calendar);
    O(Private_c, ecard_letter_data);

    printf("\n== house sub sizes ==\n");
    P(mHm_flr_c); P(mHm_lyr_c);

    printf("\n== Island_c members ==\n");
    O(Island_c, fgblock);
    O(Island_c, cottage);
    O(Island_c, flag_design);
    O(Island_c, animal);
    O(Island_c, deposit);

    printf("\n== mCD_others_c members ==\n");
    O(mCD_others_c, header);
    O(mCD_others_c, original);
    O(mCD_others_c, mail);
    O(mCD_others_c, diary);

    printf("\n== keep-block internals (relative to their own block) ==\n");
    O(mCD_keep_original_c, folder_names);
    O(mCD_keep_original_c, original);
    O(mCD_keep_mail_c, folder_names);
    O(mCD_keep_mail_c, mail);
    O(mCD_keep_diary_c, entries);
    printf("  mCD_KEEP_ORIGINAL total designs = %d\n",
           mCD_KEEP_ORIGINAL_PAGE_COUNT * mCD_KEEP_ORIGINAL_COUNT);
    printf("  mCD_KEEP_MAIL total letters     = %d\n",
           mCD_KEEP_MAIL_PAGE_COUNT * mCD_KEEP_MAIL_COUNT);
    printf("  mCD_KEEP_DIARY total entries    = %d\n",
           mCD_KEEP_DIARY_COUNT * mCD_KEEP_DIARY_ENTRY_COUNT);
    return 0;
}
