/* Generated Cutscenes Header for Flashback SMS */
#ifndef CUTSCENES_DATA_H
#define CUTSCENES_DATA_H

typedef struct {
    unsigned int nframes;
    unsigned int snapivl;
    unsigned int nsnaps;
    unsigned char data_bank0;
    const unsigned char *bg_palette;
    const unsigned char *frame_bank;
    const unsigned int *frame_ofs;
    const unsigned char *snap_bank;
    const unsigned int *snap_ofs;
    unsigned char frame_delay;
} cutscene_info_t;

/* Cutscene: logos */
extern const unsigned char bg_palette_logos[16];
extern const unsigned char frame_bank_logos[153];
extern const unsigned int  frame_ofs_logos[154];
extern const unsigned char snap_bank_logos[20];
extern const unsigned int  snap_ofs_logos[20];
extern const cutscene_info_t cutscene_logos;

/* Cutscene: intro1 */
extern const unsigned char bg_palette_intro1[16];
extern const unsigned char frame_bank_intro1[260];
extern const unsigned int  frame_ofs_intro1[261];
extern const unsigned char snap_bank_intro1[33];
extern const unsigned int  snap_ofs_intro1[33];
extern const cutscene_info_t cutscene_intro1;

/* Cutscene: intro2 */
extern const unsigned char bg_palette_intro2[16];
extern const unsigned char frame_bank_intro2[380];
extern const unsigned int  frame_ofs_intro2[381];
extern const unsigned char snap_bank_intro2[48];
extern const unsigned int  snap_ofs_intro2[48];
extern const cutscene_info_t cutscene_intro2;

/* Cutscene: holocube */
extern const unsigned char bg_palette_holocube[16];
extern const unsigned char frame_bank_holocube[35];
extern const unsigned int  frame_ofs_holocube[36];
extern const unsigned char snap_bank_holocube[5];
extern const unsigned int  snap_ofs_holocube[5];
extern const cutscene_info_t cutscene_holocube;

/* Cutscene: debut */
extern const unsigned char bg_palette_debut[16];
extern const unsigned char frame_bank_debut[50];
extern const unsigned int  frame_ofs_debut[51];
extern const unsigned char snap_bank_debut[7];
extern const unsigned int  snap_ofs_debut[7];
extern const cutscene_info_t cutscene_debut;

/* Cutscene: objet */
extern const unsigned char bg_palette_objet[16];
extern const unsigned char frame_bank_objet[35];
extern const unsigned int  frame_ofs_objet[36];
extern const unsigned char snap_bank_objet[5];
extern const unsigned int  snap_ofs_objet[5];
extern const cutscene_info_t cutscene_objet;

/* Cutscene: desinteg */
extern const unsigned char bg_palette_desinteg[16];
extern const unsigned char frame_bank_desinteg[19];
extern const unsigned int  frame_ofs_desinteg[20];
extern const unsigned char snap_bank_desinteg[3];
extern const unsigned int  snap_ofs_desinteg[3];
extern const cutscene_info_t cutscene_desinteg;

#endif
