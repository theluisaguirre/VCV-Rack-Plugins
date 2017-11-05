#include "mscHack.hpp"
#include "mscHack_Controls.hpp"
#include "dsp/digital.hpp"
#include "CLog.h"

#define GROUPS 4
#define CH_PER_GROUP 4
#define CHANNELS ( GROUPS * CH_PER_GROUP )

#define GROUP_OFF_X 52
#define CHANNEL_OFF_X 34

#define FADE_MULT (0.0005f)

#define L 0
#define R 1

#define MUTE_FADE_STATE_IDLE 0
#define MUTE_FADE_STATE_INC  1
#define MUTE_FADE_STATE_DEC  2

//-----------------------------------------------------
// Module Definition
//
//-----------------------------------------------------
struct Mix_4x4_Stereo2 : Module 
{
	enum ParamIds 
    {
        PARAM_MAIN_LEVEL,
        PARAM_LEVEL_IN,
        PARAM_PAN_IN            = PARAM_LEVEL_IN + CHANNELS,
        PARAM_GROUP_LEVEL_IN    = PARAM_PAN_IN + CHANNELS,
        PARAM_GROUP_PAN_IN      = PARAM_GROUP_LEVEL_IN + GROUPS,
        PARAM_MUTE_BUTTON       = PARAM_GROUP_PAN_IN + GROUPS,
        PARAM_SOLO_BUTTON       = PARAM_MUTE_BUTTON + CHANNELS,
        PARAM_GROUP_MUTE        = PARAM_SOLO_BUTTON + CHANNELS,
        PARAM_GROUP_SOLO        = PARAM_GROUP_MUTE + GROUPS,

        PARAM_EQ_HI             = PARAM_GROUP_SOLO + GROUPS,
        PARAM_EQ_MD             = PARAM_EQ_HI + CHANNELS,
        PARAM_EQ_LO             = PARAM_EQ_MD + CHANNELS,

        nPARAMS                 = PARAM_EQ_LO + CHANNELS
    };

	enum InputIds 
    {
        IN_LEFT,
        IN_RIGHT                = IN_LEFT + CHANNELS,
        IN_LEVEL                = IN_RIGHT + CHANNELS,
        IN_PAN                  = IN_LEVEL + CHANNELS, 
        IN_GROUP_LEVEL          = IN_PAN + CHANNELS,
        IN_GROUP_PAN            = IN_GROUP_LEVEL + GROUPS,
        IN_GROUP_MUTE           = IN_GROUP_PAN + GROUPS,
        IN_GROUP_SOLO           = IN_GROUP_MUTE + GROUPS,
        nINPUTS                 = IN_GROUP_SOLO + GROUPS 
	};

	enum OutputIds 
    {
		OUT_MAINL,
        OUT_MAINR,
        OUT_GROUPL,
        OUT_GROUPR              = OUT_GROUPL + GROUPS,

        nOUTPUTS                = OUT_GROUPR + GROUPS
	};

    CLog            lg;

    // mute buttons
    SchmittTrigger  m_SchTrigMutes[ CHANNELS ];
    float           m_fLightMutes[ CHANNELS ] = {};
    bool            m_bMuteStates[ CHANNELS ] = {};
    float           m_fMuteFade[ CHANNELS ] = {};
    
    int             m_FadeState[ CHANNELS ] = {MUTE_FADE_STATE_IDLE};

    // solo buttons
    SchmittTrigger  m_SchTrigSolos[ CHANNELS ];
    float           m_fLightSolos[ CHANNELS ] = {};
    bool            m_bSoloStates[ CHANNELS ] = {};

    // mute and solo input triggers
    SchmittTrigger  m_SchTrigInSolos[ CHANNELS ];
    SchmittTrigger  m_SchTrigInMutes[ CHANNELS ];

    // group mute buttons
    SchmittTrigger  m_SchTrigGroupMutes[ GROUPS ];
    float           m_fLightGroupMutes[ GROUPS ] = {};
    bool            m_bGroupMuteStates[ GROUPS ] = {};
    float           m_fGroupMuteFade[ GROUPS ] = {};

    int             m_GroupFadeState[ GROUPS ] = {MUTE_FADE_STATE_IDLE};

    // group solo buttons
    SchmittTrigger  m_SchTrigGroupSolos[ GROUPS ];
    float           m_fLightGroupSolos[ GROUPS ] = {};
    bool            m_bGroupSoloStates[ GROUPS ] = {};

    // group mute and solo input triggers
    SchmittTrigger  m_SchTrigGroupInSolos[ GROUPS ];
    SchmittTrigger  m_SchTrigGroupInMutes[ GROUPS ];

    // processing
    bool            m_bMono[ CHANNELS ];
    float           m_fSubMix[ GROUPS ][ 3 ] = {};

    // EQ Rez
    float           m_hpIn[ CHANNELS ];
    float           m_lpIn[ CHANNELS ];
    float           m_mpIn[ CHANNELS ];
    float           m_rezIn[ CHANNELS ] = {0};
    float           m_Freq;

#define L 0
#define R 1

    // Contructor
	Mix_4x4_Stereo2() : Module(nPARAMS, nINPUTS, nOUTPUTS){}

    // Overrides 
	void    step() override;
    json_t* toJson() override;
    void    fromJson(json_t *rootJ) override;
    void    initialize() override;
    void    randomize() override{}
    //void    reset() override;

    void ProcessMuteSolo( int channel, bool bMute, bool bGroup );
    void ProcessEQ( int ch, float *pL, float *pR );
};

//-----------------------------------------------------
// MyEQHi_Knob
//-----------------------------------------------------
struct MyEQHi_Knob : Green1_Tiny
{
    Mix_4x4_Stereo2 *mymodule;
    int param;

    void onChange() override 
    {
        mymodule = (Mix_4x4_Stereo2*)module;

        if( mymodule )
        {
            param = paramId - Mix_4x4_Stereo2::PARAM_EQ_HI;

            mymodule->m_hpIn[ param ] = value; 
        }

		RoundKnob::onChange();
	}
};

//-----------------------------------------------------
// MyEQHi_Knob
//-----------------------------------------------------
struct MyEQMid_Knob : Green1_Tiny
{
    Mix_4x4_Stereo2 *mymodule;
    int param;

    void onChange() override 
    {
        mymodule = (Mix_4x4_Stereo2*)module;

        if( mymodule )
        {
            param = paramId - Mix_4x4_Stereo2::PARAM_EQ_MD;
            mymodule->m_mpIn[ param ] = value; 
        }

		RoundKnob::onChange();
	}
};

//-----------------------------------------------------
// MyEQHi_Knob
//-----------------------------------------------------
struct MyEQLo_Knob : Green1_Tiny
{
    Mix_4x4_Stereo2 *mymodule;
    int param;

    void onChange() override 
    {
        mymodule = (Mix_4x4_Stereo2*)module;

        if( mymodule )
        {
            param = paramId - Mix_4x4_Stereo2::PARAM_EQ_LO;
            mymodule->m_lpIn[ param ] = value; 
        }

		RoundKnob::onChange();
	}
};
#define CUTOFF (0.025f)
//-----------------------------------------------------
// Procedure:   Widget
//
//-----------------------------------------------------
Mix_4x4_Stereo2_Widget::Mix_4x4_Stereo2_Widget() 
{
    float fx, fx2, fx3, fx5, fx7;
    int ch, x, y, i, ybase;
	Mix_4x4_Stereo2 *module = new Mix_4x4_Stereo2();
	setModule(module);
	box.size = Vec( 15*47, 380);

	{
		SVGPanel *panel = new SVGPanel();
		panel->box.size = box.size;
		panel->setBackground(SVG::load(assetPlugin(plugin, "res/Mix_4x4_Stereo2.svg")));
		addChild(panel);
	}

    //module->lg.Open("Mix_4x4_Stereo.txt");

	addChild(createScrew<ScrewSilver>(Vec(15, 0)));
	addChild(createScrew<ScrewSilver>(Vec(box.size.x-30, 0)));
	addChild(createScrew<ScrewSilver>(Vec(15, 365))); 
	addChild(createScrew<ScrewSilver>(Vec(box.size.x-30, 365)));

    //addInput(createInput<MyPortInSmall>( Vec( 6, 40 ), module, Mix_4x4_Stereo::IN_WAVE ) );
    //addOutput(createOutput<MyPortOutSmall>( Vec( 50, 40 ), module, Mix_4x4_Stereo::OUT_WAVE ) );

    //----------------------------------------------------
    // Add mix sliders
    x = 23;
    y = 38;

    // main channel
	for ( ch = 0; ch < CHANNELS; ch++ ) 
    {
        // Left channel inputs
        addInput(createInput<MyPortInSmall>( Vec( x, y ), module, Mix_4x4_Stereo2::IN_LEFT + ch ) );

        y += 25;

        // Right channel inputs
        addInput(createInput<MyPortInSmall>( Vec( x, y ), module, Mix_4x4_Stereo2::IN_RIGHT + ch ) );

        y += 26;

        // Level knobs
        addParam(createParam<Blue2_Small>( Vec( x - 5, y ), module, Mix_4x4_Stereo2::PARAM_LEVEL_IN + ch, 0.0, 2.0, 0.0 ) );

        y += 31;

        // Level inputs
        addInput(createInput<MyPortInSmall>( Vec( x, y ), module, Mix_4x4_Stereo2::IN_LEVEL + ch ) );

        y += 23;

        // pan knobs
        addParam(createParam<Yellow2_Small>( Vec( x - 5, y ), module, Mix_4x4_Stereo2::PARAM_PAN_IN + ch, -1.0, 1.0, 0.0 ) );

        y += 31;

        // Pan inputs
        addInput(createInput<MyPortInSmall>( Vec( x, y ), module, Mix_4x4_Stereo2::IN_PAN + ch ) );

        y += 22;

        // mute buttons
        addParam(createParam<MySquareButton2>( Vec( x - 7, y ), module, Mix_4x4_Stereo2::PARAM_MUTE_BUTTON + ch, 0.0, 1.0, 0.0 ) );
        addChild(createValueLight<SmallLight<RedValueLight>>( Vec( x - 4, y + 5 ), &module->m_fLightMutes[ ch ] ) );

        //y += 26;

        // solo buttons
        addParam(createParam<MySquareButton2>( Vec( x + 9, y ), module, Mix_4x4_Stereo2::PARAM_SOLO_BUTTON + ch, 0.0, 1.0, 0.0 ) );
        addChild(createValueLight<SmallLight<GreenValueLight>>( Vec( x + 12, y + 5 ), &module->m_fLightSolos[ ch ] ) );

        y += 22;

        // eq and rez
        addParam(createParam<MyEQHi_Knob>( Vec( x, y ), module, Mix_4x4_Stereo2::PARAM_EQ_HI + ch, 0.0, 1.0, 0.5 ) );

        y += 19;

        addParam(createParam<MyEQMid_Knob>( Vec( x, y ), module, Mix_4x4_Stereo2::PARAM_EQ_MD + ch, 0.0, 1.0, 0.5 ) );
        
        y += 19;
        
        addParam(createParam<MyEQLo_Knob>( Vec( x, y ), module, Mix_4x4_Stereo2::PARAM_EQ_LO + ch, 0.0, 1.0, 0.5 ) );
        
        if( ( ch & 3 ) == 3 )
        {
            x += GROUP_OFF_X;
        }
        else
        {
            x += CHANNEL_OFF_X;
        }

        y = 39;
    }

    // group mixera
    ybase = 278;
    x = 21;
    for( i = 0; i < GROUPS; i++ )
    {
        // mute/solo inputs
        y = ybase + 23;
        addInput(createInput<MyPortInSmall>( Vec( x, y ), module, Mix_4x4_Stereo2::IN_GROUP_MUTE + i ) );

        y += 30;

        addInput(createInput<MyPortInSmall>( Vec( x, y ), module, Mix_4x4_Stereo2::IN_GROUP_SOLO + i ) );

        // mute/solo buttons
        x += 23;
        y = ybase + 21;

        addParam(createParam<MySquareButton2>( Vec( x, y ), module, Mix_4x4_Stereo2::PARAM_GROUP_MUTE + i, 0.0, 1.0, 0.0 ) );
        addChild(createValueLight<SmallLight<RedValueLight>>( Vec( x + 3, y + 4 ), &module->m_fLightGroupMutes[ i ] ) );

        y += 30;

        addParam(createParam<MySquareButton2>( Vec( x, y ), module, Mix_4x4_Stereo2::PARAM_GROUP_SOLO + i, 0.0, 1.0, 0.0 ) );
        addChild(createValueLight<SmallLight<GreenValueLight>>( Vec( x + 3, y + 4 ), &module->m_fLightGroupSolos[ i ] ) );

        // group level and pan inputs
        x += 24;
        y = ybase + 11;

        addInput(createInput<MyPortInSmall>( Vec( x, y ), module, Mix_4x4_Stereo2::IN_GROUP_LEVEL + i ) );

        y += 40;

        addInput(createInput<MyPortInSmall>( Vec( x, y ), module, Mix_4x4_Stereo2::IN_GROUP_PAN + i ) );

        // group level and pan knobs
        x += 24;
        y = ybase + 6;

        addParam(createParam<Blue2_Small>( Vec( x, y ), module, Mix_4x4_Stereo2::PARAM_GROUP_LEVEL_IN + i, 0.0, 2.0, 0.0 ) );

        y += 39;

        addParam(createParam<Yellow2_Small>( Vec( x, y ), module, Mix_4x4_Stereo2::PARAM_GROUP_PAN_IN + i, -1.0, 1.0, 0.0 ) );

        // group outputs
        x += 36;
        y = ybase + 29;

        addOutput(createOutput<MyPortOutSmall>( Vec( x, y ), module, Mix_4x4_Stereo2::OUT_GROUPL + i ) );

        y += 31;

        addOutput(createOutput<MyPortOutSmall>( Vec( x, y ), module, Mix_4x4_Stereo2::OUT_GROUPR + i ) );

        // account for slight error in pixel conversion to svg area
        x += 45 + ( i * 2 );
    }

    // main mixer knob 
    addParam(createParam<Blue2_Big>( Vec( 633, 237 ), module, Mix_4x4_Stereo2::PARAM_MAIN_LEVEL, 0.0, 2.0, 0.0 ) );

    // outputs
    addOutput(createOutput<MyPortOutSmall>( Vec( 636, 305 ), module, Mix_4x4_Stereo2::OUT_MAINL ) );
    addOutput(createOutput<MyPortOutSmall>( Vec( 668, 335 ), module, Mix_4x4_Stereo2::OUT_MAINR ) );

    // calculate eq rez freq
    fx = 3.141592 * (CUTOFF * 0.026315789473684210526315789473684) * 2 * 3.141592; 
    fx2 = fx*fx;
    fx3 = fx2*fx; 
    fx5 = fx3*fx2; 
    fx7 = fx5*fx2;

    module->m_Freq = 2.0 * (fx 
	    - (fx3 * 0.16666666666666666666666666666667) 
	    + (fx5 * 0.0083333333333333333333333333333333) 
	    - (fx7 * 0.0001984126984126984126984126984127));

    initialize();
}

//-----------------------------------------------------
// Procedure:   initialize
//
//-----------------------------------------------------
void Mix_4x4_Stereo2::initialize()
{
    int ch, i;

    for( ch = 0; ch < CHANNELS; ch++ )
    {
        m_FadeState[ ch ] = MUTE_FADE_STATE_IDLE;
        m_fLightMutes[ ch ] = 0.0;
        m_fLightSolos[ ch ] = 0.0;
        m_bMuteStates[ ch ] = false;
        m_bSoloStates[ ch ] = false;
        m_fMuteFade[ ch ] = 1.0;
    }

    for( i = 0; i < GROUPS; i++ )
    {
        m_GroupFadeState[ i ] = MUTE_FADE_STATE_IDLE;
        m_fLightGroupMutes[ i ] = 0.0;
        m_fLightGroupSolos[ i ] = 0.0;
        m_bGroupMuteStates[ i ] = false;
        m_bGroupSoloStates[ i ] = false;
        m_fGroupMuteFade[ i ] = 1.0;
    }

    params[ PARAM_MAIN_LEVEL ].value = 0.0;
}

//-----------------------------------------------------
// Procedure:   
//
//-----------------------------------------------------
json_t *Mix_4x4_Stereo2::toJson() 
{
    bool *pbool;
    json_t *gatesJ;
	json_t *rootJ = json_object();

	// channel mutes
    pbool = &m_bMuteStates[ 0 ];

	gatesJ = json_array();

	for (int i = 0; i < CHANNELS; i++)
    {
		json_t *gateJ = json_integer( (int) pbool[ i ] );
		json_array_append_new( gatesJ, gateJ );
	}

	json_object_set_new( rootJ, "channel mutes", gatesJ );

	// channel solos
    pbool = &m_bSoloStates[ 0 ];

	gatesJ = json_array();

	for (int i = 0; i < CHANNELS; i++)
    {
		json_t *gateJ = json_integer( (int) pbool[ i ] );
		json_array_append_new( gatesJ, gateJ );
	}

	json_object_set_new( rootJ, "channel solos", gatesJ );

	// group mutes
    pbool = &m_bGroupMuteStates[ 0 ];

	gatesJ = json_array();

	for (int i = 0; i < GROUPS; i++)
    {
		json_t *gateJ = json_integer( (int) pbool[ i ] );
		json_array_append_new( gatesJ, gateJ );
	}

	json_object_set_new( rootJ, "group mutes", gatesJ );

	// group solos
    pbool = &m_bGroupSoloStates[ 0 ];

	gatesJ = json_array();

	for (int i = 0; i < GROUPS; i++)
    {
		json_t *gateJ = json_integer( (int) pbool[ i ] );
		json_array_append_new( gatesJ, gateJ );
	}

	json_object_set_new( rootJ, "group solos", gatesJ );

	return rootJ;
}

//-----------------------------------------------------
// Procedure:   fromJson
//
//-----------------------------------------------------
void Mix_4x4_Stereo2::fromJson(json_t *rootJ) 
{
    int ch, i;
    bool *pbool;
    json_t *StepsJ;

	// channel mutes
    pbool = &m_bMuteStates[ 0 ];

	StepsJ = json_object_get( rootJ, "channel mutes" );

	if (StepsJ) 
    {
		for ( i = 0; i < CHANNELS; i++)
        {
			json_t *gateJ = json_array_get(StepsJ, i);

			if (gateJ)
				pbool[ i ] = json_integer_value( gateJ );
		}
	}

	// channel solos
    pbool = &m_bSoloStates[ 0 ];

	StepsJ = json_object_get( rootJ, "channel solos" );

	if (StepsJ) 
    {
		for ( i = 0; i < CHANNELS; i++)
        {
			json_t *gateJ = json_array_get(StepsJ, i);

			if (gateJ)
				pbool[ i ] = json_integer_value( gateJ );
		}
	}

	// group mutes
    pbool = &m_bGroupMuteStates[ 0 ];

	StepsJ = json_object_get( rootJ, "group mutes" );

	if (StepsJ) 
    {
		for ( i = 0; i < GROUPS; i++)
        {
			json_t *gateJ = json_array_get(StepsJ, i);

			if (gateJ)
				pbool[ i ] = json_integer_value( gateJ );
		}
	}

	// group solos
    pbool = &m_bGroupSoloStates[ 0 ];

	StepsJ = json_object_get( rootJ, "group solos" );

	if (StepsJ) 
    {
		for ( i = 0; i < GROUPS; i++)
        {
			json_t *gateJ = json_array_get(StepsJ, i);

			if (gateJ)
				pbool[ i ] = json_integer_value( gateJ );
		}
	}

    for( ch = 0; ch < CHANNELS; ch++ )
    {
        m_fMuteFade[ ch ] = m_bMuteStates[ ch ] ? 0.0: 1.0;
        m_fLightMutes[ ch ] = m_bMuteStates[ ch ] ? 1.0: 0.0;

        m_fLightSolos[ ch ] = m_bSoloStates[ ch ] ? 1.0: 0.0;
    }

    for( i = 0; i < GROUPS; i++ )
    {
        m_fGroupMuteFade[ i ] = m_bGroupMuteStates[ i ] ? 0.0: 1.0;
        m_fLightGroupMutes[ i ] = m_bGroupMuteStates[ i ] ? 1.0: 0.0;

        m_fLightGroupSolos[ i ] = m_bGroupSoloStates[ i ] ? 1.0: 0.0;
    }
}

//-----------------------------------------------------
// Procedure:   ProcessMuteSolo
//
//-----------------------------------------------------
void Mix_4x4_Stereo2::ProcessMuteSolo( int index, bool bMute, bool bGroup )
{
    int i, group, si, ei;
    bool bSoloEnabled = false, bSoloOff = false;

    if( bGroup )
    {
        if( bMute )
        {
            m_bGroupMuteStates[ index ] = !m_bGroupMuteStates[ index ];

            // turn solo off
            if( m_bGroupSoloStates[ index ] )
            {
                bSoloOff = true;
                m_bGroupSoloStates[ index ] = false;
                m_fLightGroupSolos[ index ] = 0.0;
            }

            // if mute is off then set volume
            if( m_bGroupMuteStates[ index ] )
            {
                m_fLightGroupMutes[ index ] = 1.0;
                m_GroupFadeState[ index ] = MUTE_FADE_STATE_DEC;
            }
            else
            {
                m_fLightGroupMutes[ index ] = 0.0;
                m_GroupFadeState[ index ] = MUTE_FADE_STATE_INC;
            }
        }
        else
        {
            m_bGroupSoloStates[ index ] = !m_bGroupSoloStates[ index ];

            // turn mute off
            if( m_bGroupMuteStates[ index ] )
            {
                m_bGroupMuteStates[ index ] = false;
                m_fLightGroupMutes[ index ] = 0.0;
            }

            // shut down volume of all groups not in solo
            if( !m_bGroupSoloStates[ index ] )
            {
                bSoloOff = true;
                m_fLightGroupSolos[ index ] = 0.0;
            }
            else
            {
                m_fLightGroupSolos[ index ] = 1.0;
            }
        }

        // is a track soloing?
        for( i = 0; i < GROUPS; i++ )
        {
            if( m_bGroupSoloStates[ i ] )
            {
                bSoloEnabled = true;
                break;
            }
        }

        if( bSoloEnabled )
        {
            // process solo
            for( i = 0; i < GROUPS; i++ )
            {
                // shut down volume of all groups not in solo
                if( !m_bGroupSoloStates[ i ] )
                {
                    m_GroupFadeState[ i ] = MUTE_FADE_STATE_DEC;
                }
                else
                {
                    m_GroupFadeState[ i ] = MUTE_FADE_STATE_INC;
                }
            }
        }
        // nobody soloing and just turned solo off then enable all channels that aren't muted
        else if( bSoloOff )
        {
            // process solo
            for( i = 0; i < GROUPS; i++ )
            {
                // bring back if not muted
                if( !m_bGroupMuteStates[ i ] )
                {
                    m_GroupFadeState[ i ] = MUTE_FADE_STATE_INC;
                }
            }
        }
    }
    // !bGroup
    else
    {
        group = index / GROUPS;

        si = group * 4;
        ei = si + CH_PER_GROUP;
        
        if( bMute )
        {
            m_bMuteStates[ index ] = !m_bMuteStates[ index ];

            // turn solo off
            if( m_bSoloStates[ index ] )
            {
                bSoloOff = true;
                m_bSoloStates[ index ] = false;
                m_fLightSolos[ index ] = 0.0;
            }

            // if mute is off then set volume
            if( m_bMuteStates[ index ] )
            {
                m_fLightMutes[ index ] = 1.0;
                m_FadeState[ index ] = MUTE_FADE_STATE_DEC;
            }
            else
            {
                m_fLightMutes[ index ] = 0.0;
                m_FadeState[ index ] = MUTE_FADE_STATE_INC;
            }
        }
        else
        {
            m_bSoloStates[ index ] = !m_bSoloStates[ index ];

            // turn mute off
            if( m_bMuteStates[ index ] )
            {
                m_bMuteStates[ index ] = false;
                m_fLightMutes[ index ] = 0.0;
            }

            // toggle solo
            if( !m_bSoloStates[ index ] )
            {
                bSoloOff = true;
                m_fLightSolos[ index ] = 0.0;
            }
            else
            {
                m_fLightSolos[ index ] = 1.0;
            }
        }

        // is a track soloing?
        for( i = si; i < ei; i++ )
        {
            if( m_bSoloStates[ i ] )
            {
                bSoloEnabled = true;
                break;
            }
        }

        if( bSoloEnabled )
        {
            // process solo
            for( i = si; i < ei; i++ )
            {
                // shut down volume of all not in solo
                if( !m_bSoloStates[ i ] )
                {
                    m_FadeState[ i ] = MUTE_FADE_STATE_DEC;
                }
                else
                {
                    m_FadeState[ i ] = MUTE_FADE_STATE_INC;
                }
            }
        }
        // nobody soloing and just turned solo off then enable all channels that aren't muted
        else if( bSoloOff )
        {
            // process solo
            for( i = si; i < ei; i++ )
            {
                // bring back if not muted
                if( !m_bMuteStates[ i ] )
                {
                    m_FadeState[ i ] = MUTE_FADE_STATE_INC;
                }
            }
        }
    }
}

//-----------------------------------------------------
// Procedure:   ProcessEQ
//
//-----------------------------------------------------
#define MULTI (0.33333333333333333333333333333333f)
void Mix_4x4_Stereo2::ProcessEQ( int ch, float *pL, float *pR )
{
    float rez, hp1; 
    static float lp1[ CHANNELS ][ 2 ] = {0.0}, bp1[ CHANNELS ][ 2 ] = {0.0}; 
    float input[ 2 ], out[ 2 ], lowpass, bandpass, highpass;

    input[ L ] = *pL / 5.0;
    input[ R ] = *pR / 5.0;

    rez = 1.00;

    // do left and right channels
    for( int i = 0; i < 2; i++ )
    {
        input[ i ] = input[ i ] + 0.000000001;

        lp1[ ch ][ i ] = lp1[ ch ][ i ] + m_Freq * bp1[ ch ][ i ]; 
        hp1 = input[ i ] - lp1[ ch ][ i ] - rez * bp1[ ch ][ i ]; 
        bp1[ ch ][ i ] = m_Freq * hp1 + bp1[ ch ][ i ]; 
        lowpass  = lp1[ ch ][ i ]; 
        highpass = hp1; 
        bandpass = bp1[ ch ][ i ]; 

        lp1[ ch ][ i ] = lp1[ ch ][ i ] + m_Freq * bp1[ ch ][ i ]; 
        hp1 = input[ i ] - lp1[ ch ][ i ] - rez * bp1[ ch ][ i ]; 
        bp1[ ch ][ i ] = m_Freq * hp1 + bp1[ ch ][ i ]; 
        lowpass  = lowpass  + lp1[ ch ][ i ]; 
        highpass = highpass + hp1; 
        bandpass = bandpass + bp1[ ch ][ i ]; 

        input[ i ] = input[ i ] - 0.000000001;
        lp1[ ch ][ i ] = lp1[ ch ][ i ] + m_Freq * bp1[ ch ][ i ]; 
        hp1 = input[ i ] - lp1[ ch ][ i ] - rez * bp1[ ch ][ i ]; 
        bp1[ ch ][ i ] = m_Freq * hp1 + bp1[ ch ][ i ]; 

        lowpass  = (lowpass  + lp1[ ch ][ i ]) * MULTI; 
        highpass = (highpass + hp1) * MULTI; 
        bandpass = (bandpass + bp1[ ch ][ i ]) * MULTI;

        out[ i ] = ( highpass * m_hpIn[ ch ] ) + ( lowpass * m_lpIn[ ch ] ) + ( bandpass * m_mpIn[ ch ] );
    }

    *pL = clampf( out[ L ] * 5.0, -5.0, 5.0 );
    *pR = clampf( out[ R ] * 5.0, -5.0, 5.0 );
}

//-----------------------------------------------------
// Procedure:   step
//
//-----------------------------------------------------
void Mix_4x4_Stereo2::step() 
{
    int ch, group;
    float inL = 0.0, inR = 0.0, outL, outR, pan, mainL = 0.0, mainR = 0.0;
    bool bGroupActive[ 4 ] = {0};

    memset( m_fSubMix, 0, sizeof(m_fSubMix) );

    // channel mixers
	for ( ch = 0; ch < CHANNELS; ch++ ) 
    {
        group = ch / 4;

        inL = 0.0;
        inR = 0.0;

        if( inputs[ IN_RIGHT + ch ].active || inputs[ IN_LEFT + ch ].active )
        {
            bGroupActive[ group ] = true;

            // process mute buttons
            if( m_SchTrigMutes[ ch ].process( params[ PARAM_MUTE_BUTTON + ch ].value ) )
            {
                ProcessMuteSolo( ch, true, false );
            }

            // process solo buttons
            if( m_SchTrigSolos[ ch ].process( params[ PARAM_SOLO_BUTTON + ch ].value ) )
            {
                ProcessMuteSolo( ch, false, false );
            }

            // check right channel first for possible mono
            if( inputs[ IN_RIGHT + ch ].active )
            {
                inR = inputs[ IN_RIGHT + ch ].value * clampf( ( params[ PARAM_LEVEL_IN + ch ].value + ( inputs[ IN_LEVEL + ch ].normalize( 0.0 ) / 10.0 ) ), -1.0, 1.0 ); 
                m_bMono[ ch ] = false;
            }
            else
                m_bMono[ ch ] = true;

            // left channel
            if( inputs[ IN_LEFT + ch ].active )
            {
                inL = inputs[ IN_LEFT + ch ].value * clampf( ( params[ PARAM_LEVEL_IN + ch ].value + ( inputs[ IN_LEVEL + ch ].normalize( 0.0 ) / 10.0 ) ), -1.0, 1.0 ); 

                if( m_bMono[ ch ] )
                    inR = inL;
            }

            if( m_FadeState[ ch ] == MUTE_FADE_STATE_DEC )
            {
                m_fMuteFade[ ch ] -= FADE_MULT;

                if( m_fMuteFade[ ch ] <= 0.0 )
                {
                    m_fMuteFade[ ch ] = 0.0;
                    m_FadeState[ ch ] = MUTE_FADE_STATE_IDLE;
                }
            }
            else if( m_FadeState[ ch ] == MUTE_FADE_STATE_INC )
            {
                m_fMuteFade[ ch ] += FADE_MULT;

                if( m_fMuteFade[ ch ] >= 1.0 )
                {
                    m_fMuteFade[ ch ] = 1.0;
                    m_FadeState[ ch ] = MUTE_FADE_STATE_IDLE;
                }
            }

            ProcessEQ( ch, &inL, &inR );

            inL *= m_fMuteFade[ ch ];
            inR *= m_fMuteFade[ ch ];

            // pan
            pan = clampf( params[ PARAM_PAN_IN + ch ].value + ( inputs[ IN_PAN + ch ].normalize( 0.0 ) / 10.0 ), -1.0, 1.0 );

            //lg.f("pan = %.3f\n", inputs[ IN_PAN + ch ].value );

            if( pan <= 0.0 )
                inR *= ( 1.0 + pan );
            else
                inL *= ( 1.0 - pan );
        }
        // this channel not active
        else
        {

        }

        m_fSubMix[ group ][ L ] += inL;
        m_fSubMix[ group ][ R ] += inR;
    }

    // group mixers
	for ( group = 0; group < GROUPS; group++ ) 
    {
        outL = 0.0;
        outR = 0.0;

        if( bGroupActive[ group ] )
        {
		    if( inputs[ IN_GROUP_MUTE + group ].active ) 
            {
			    // External clock
			    if( m_SchTrigGroupInMutes[ group ].process( inputs[ IN_GROUP_MUTE + group ].value ) ) 
                {
				    ProcessMuteSolo( group, true, true );
    		    }
		    }

		    if( inputs[ IN_GROUP_SOLO + group ].active ) 
            {
			    // External clock
			    if( m_SchTrigGroupInSolos[ group ].process( inputs[ IN_GROUP_SOLO + group ].value ) ) 
                {
				    ProcessMuteSolo( group, false, true );
    		    }
		    }

            // process mute buttons
            if( m_SchTrigGroupMutes[ group ].process( params[ PARAM_GROUP_MUTE + group ].value ) )
            {
                ProcessMuteSolo( group, true, true );
            }

            if( m_SchTrigGroupSolos[ group ].process( params[ PARAM_GROUP_SOLO + group ].value ) )
            {
                ProcessMuteSolo( group, false, true );
            }

            outL = m_fSubMix[ group ][ L ] * clampf( params[ PARAM_GROUP_LEVEL_IN + group ].value + ( inputs[ IN_GROUP_LEVEL + group ].normalize( 0.0 ) / 10.0 ), -1.0, 1.0 );
            outR = m_fSubMix[ group ][ R ] * clampf( params[ PARAM_GROUP_LEVEL_IN + group ].value + ( inputs[ IN_GROUP_LEVEL + group ].normalize( 0.0 ) / 10.0 ), -1.0, 1.0 );

            // pan
            pan = clampf( params[ PARAM_GROUP_PAN_IN + group ].value + ( inputs[ IN_GROUP_PAN + group ].normalize( 0.0 ) / 10.0 ), -1.0, 1.0 );

            if( pan <= 0.0 )
                outR *= ( 1.0 + pan );
            else
                outL *= ( 1.0 - pan );

            if( m_GroupFadeState[ group ] == MUTE_FADE_STATE_DEC )
            {
                m_fGroupMuteFade[ group ] -= FADE_MULT;

                if( m_fGroupMuteFade[ group ] <= 0.0 )
                {
                    m_fGroupMuteFade[ group ] = 0.0;
                    m_GroupFadeState[ group ] = MUTE_FADE_STATE_IDLE;
                }
            }
            else if( m_GroupFadeState[ group ] == MUTE_FADE_STATE_INC )
            {
                m_fGroupMuteFade[ group ] += FADE_MULT;

                if( m_fGroupMuteFade[ group ] >= 1.0 )
                {
                    m_fGroupMuteFade[ group ] = 1.0;
                    m_GroupFadeState[ group ] = MUTE_FADE_STATE_IDLE;
                }
            }

            outL *= m_fGroupMuteFade[ group ];
            outR *= m_fGroupMuteFade[ group ];

            outputs[ OUT_GROUPL + group ].value = outL;
            outputs[ OUT_GROUPR + group ].value = outR;
        }

        mainL += outL;
        mainR += outR;
    }

    outputs[ OUT_MAINL ].value = mainL * params[ PARAM_MAIN_LEVEL ].value;
    outputs[ OUT_MAINR ].value = mainR * params[ PARAM_MAIN_LEVEL ].value;
}