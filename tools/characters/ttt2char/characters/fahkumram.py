"""Fahkumram from Tekken 7 (PC), in his default costume.

Only names, paths and rules live here; every byte of his model, textures and moveset is read from the player's own
Tekken 7 and Tekken Tag Tournament 2 files when the importer runs. His materials are classified by the importer
(model.classify); MATERIALS lists only exceptions.
"""
NAME = 'Fahkumram'
SHORT_NAME = 'FAHKUMRAM'
TEKKEN7_ID = 54
TEKKEN7_CODE = 'nsc'
CODE = 'nsc'
SEX = 'male'

DONOR = 'lar'                 # Lars Alexandersson: a slim male skeleton
DONOR_ROW = 38

_ITEM = '/Game/Character/Item'
TEKKEN7 = {
    'costume': _ITEM + '/Sets/NSC/CS_NSC_DefaultCos1P',
    'costume_2p': _ITEM + '/Sets/NSC/CS_NSC_DefaultCos2P',
    'parts': [
        ('fac', _ITEM + '/Meshes/nsc/Face/nsc_fac/Meshes/SK_CH_nsc_fac'),
        ('bdl', _ITEM + '/Meshes/nsc/Lower/nsc_bdl_1p/Meshes/SK_CH_nsc_bdl_1p'),
        ('bdu', _ITEM + '/Meshes/nsc/Upper/nsc_bdu_1p/Meshes/SK_CH_nsc_bdu_1p'),
        ('har', _ITEM + '/Meshes/nsc/Hair/nsc_har_1p/Meshes/SK_CH_nsc_har_1p'),
        ('fah', _ITEM + '/Meshes/nsc/Face_Hair/nsc_fah_1p/Meshes/SK_CH_nsc_fah_1p'),
    ],
    'merge_order': ['fac', 'bdl', 'bdu', 'har', 'fah'],
}

MESHES = {
    'fac': ('nsc_fac', 'ATAMA_Joint', 0),
    'bdl': ('nsc_bdl', 'KOSHI_jnt', 1),
    'bdu': ('nsc_bdu', 'MUNE2_jnt', 2),
    'har': ('nsc_har', 'ATAMA_jnt_assist', 3),
    'fah': ('nsc_fah', 'ATAMA_jnt_assist', 4),
}

MATERIALS = {}

# the material templates: Jun's (they are only shaders; the textures are his)
TEMPLATES = {
    'skin': ('jun.nud', 'ude_A_O_OBJ', 0),
    'mouth': ('jun.nud', 'cha:kao_O_OBJ', 0),
    'eye': ('jun.nud', 'cha:kao_O_OBJ', 2),
    'overlay': ('jun.nud', 'cha:kao_O_OBJ', 7),
    'cloth': (0xD4F28D09, 'jun_upb_1p_b', 0),
    'hair': (0x5F412CA4, 'jun_hed_1p', 1),
    'colour': (0xD4F28D09, 'jun_upb_1p_b', 2),
    'colour_alpha': (0xD4F28D09, 'jun_upb_1p_b', 1),
}
TEMPLATE_TEXTURES = ['jun.nut', 0x24F8AE59, 0x277C7A37, 0xAD9B60F0, 0x900FD8C4]

_UI = '/Game/UI_Common/Texture2D/ReplaceImages'
ART = {
    'bust': (_UI + '/VS_CH_L/VS_CH_L_NSC_7FR_DEF', (427, 133, 145)),
    'body': (_UI + '/CS_CH_L/CS_CH_L_NSC', (636, 194, 105)),
    'name': _UI + '/CS_CH_NAME/CS_CH_NAME_NSC',
    'costume': _UI + '/CS_CH_SLOT/CS_CH_SLOT_NSC_01',
}

VOICE_PREFIX = 'nc'

# An invisible hand point lowers b+1 collision by 40 mm; no drawn mesh.
WEAPONS = [{'bone': 'L_Hand', 'parent': 'TE_L_jnt', 'pair': None,
            'points': {0xfe: {'world_offset': (0, -40, 0)}}}]

# the id maps: Heihachi in both games, and his generic moves against TTT2's (Heihachi, the donor, Asuka)
ALIGN = [('heihachi', 13), ('self', 13), ('heihachi', 14), ('self', DONOR_ROW), ('heihachi', 9), ('self', 23)]
MOVESET = {
    'direct_buffered_strings': ('Nc_6lk',),
    'hitboxes': {'Nc_lp00B': (0xfe, 0, 0xfe, 11)},
    'air_reactions': {'Nc_lp00B': 'J8_jprk00',
                      'Nc_66rklkrp': (23, 'Jn_mc03s')},
    'tag_launchers': {'Nc_66rklkrp': 'La_Stormgear'},
    'tag_launcher_air_only': ('Nc_66rklkrp',),
    'electric_tag_exit': True,
    'tag_exit_name': 'Fahkumram_Hellsweep_TagExit',
    'air_reaction_reference': 40,  # Jinpachi u+4, airborne reaction
    'bounds': {'Nc_66rklp': True, 'Nc_3rplp': True},
    'name': '[FAHKUMRAM]',
    # Preserve Fahk's running 3 (ncss_mach) and d+4 (ncss_2rk), despite generic names.
    'own_moves': r'^(Co_runlk00|Co_t_srk00)$',
    'drop_moves': r'^Nc_Direct$',
    'direct': [(r'^(la|mk)_s', 'anim', ['nc_s%02d' % k for k in range(4)]),     # the donor's intros -> his
               (r'^la_w', 'anim', ['nc_w%02d' % k for k in range(3)])],         # win poses
}
