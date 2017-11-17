#include "mscHack.hpp"
#include "mscHack_Controls.hpp"
#include "dsp/digital.hpp"
#include "CLog.h"

#define nCHANNELS 3
#define CHANNEL_H 90
#define CHANNEL_Y 65
#define CHANNEL_X 10

#define MAX_nWAVES 7
#define MAX_DETUNE 100 //Hz

#define freqMAX 300.0f
#define ADS_MAX_TIME_SECONDS 0.5f

#define WAVE_BUFFER_LEN ( 192000 / 20 ) // (9600) based on quality for 20Hz at max sample rate 192000

typedef struct
{
    int   state;
    int   a, d, r;
    int   acount, dcount, rcount;
    float fainc, frinc;
    float out;
    bool  bTrig;
}ADR_STRUCT;

typedef struct
{
    int   wavetype;
    int   filtertype;

    // wave
    float phase[ MAX_nWAVES ];
    float freq[ MAX_nWAVES ];

    //filter
    float q, f;

    float lp1[ 2 ] = {}, bp1[ 2 ] = {};

    // ads
    ADR_STRUCT adr_wave;
    
}OSC_PARAM_STRUCT;

//-----------------------------------------------------
// Module Definition
//
//-----------------------------------------------------
struct Osc_3Ch : Module 
{
    enum WaveTypes
    {
        WAVE_SIN,
        WAVE_TRI,
        WAVE_SQR,
        WAVE_SAW,
        WAVE_NOISE,
        nWAVEFORMS
    };

	enum ParamIds 
    {
        PARAM_DELAY,
        PARAM_ATT       = PARAM_DELAY + nCHANNELS,
        PARAM_REL       = PARAM_ATT + nCHANNELS,
        PARAM_REZ       = PARAM_REL + nCHANNELS,
        PARAM_WAVES     = PARAM_REZ + nCHANNELS,
        PARAM_CUTOFF    = PARAM_WAVES + (nWAVEFORMS * nCHANNELS),
        PARAM_RES       = PARAM_CUTOFF + nCHANNELS,
        PARAM_OUTLVL    = PARAM_RES + nCHANNELS,
        PARAM_FILTER_MODE = PARAM_OUTLVL + nCHANNELS,
        PARAM_nWAVES    = PARAM_FILTER_MODE + nCHANNELS,
        PARAM_SPREAD    = PARAM_nWAVES + nCHANNELS,
        PARAM_DETUNE    = PARAM_SPREAD + nCHANNELS,
        nPARAMS         = PARAM_DETUNE + nCHANNELS
    };

	enum InputIds 
    {
        IN_VOCT,
        IN_TRIG         = IN_VOCT + nCHANNELS,
        IN_FILTER       = IN_TRIG + nCHANNELS,
        IN_REZ          = IN_FILTER + nCHANNELS,
        IN_LEVEL        = IN_REZ + nCHANNELS,
        nINPUTS         = IN_LEVEL + nCHANNELS,
	};

	enum OutputIds 
    {
        OUTPUT_AUDIO,
        nOUTPUTS        = OUTPUT_AUDIO + (nCHANNELS * 2)
	};

    enum ADRSTATES
    {
        ADR_OFF,
        ADR_WAIT_PHASE,
        ADR_FADE_OUT,
        ADR_ATTACK,
        ADR_DELAY,
        ADR_RELEASE
    };

    enum FILTER_TYPES
    {
        FILTER_OFF,
        FILTER_LP,
        FILTER_HP,
        FILTER_BP,
     };

    CLog            lg;

    SchmittTrigger   m_SchTrig[ nCHANNELS ];

    OSC_PARAM_STRUCT m_Wave[ nCHANNELS ] = {};

    // waveforms
    float           m_BufferWave[ nWAVEFORMS ][ WAVE_BUFFER_LEN ] = {};
    float           m_fLightWaveSelect[ nCHANNELS ][ nWAVEFORMS ] = {};

    float           m_DetuneIn[ nCHANNELS ] = {};
    float           m_Detune[ nCHANNELS ][ MAX_nWAVES ][ MAX_nWAVES ];

    float           m_SpreadIn[ nCHANNELS ] = {};
    float           m_Pan[ nCHANNELS ][ MAX_nWAVES ][ MAX_nWAVES ][ 2 ];

    int             m_nWaves[ nCHANNELS ] = {};

    // Contructor
	Osc_3Ch() : Module(nPARAMS, nINPUTS, nOUTPUTS){}

    //-----------------------------------------------------
    // MyWaveButton
    //-----------------------------------------------------
    struct MyWaveButton : MySquareButton
    {
        int ch, but;
        Osc_3Ch *mymodule;
        int param;

        void onChange() override 
        {
            mymodule = (Osc_3Ch*)module;

            if( mymodule && value == 1.0 )
            {
                param = paramId - Osc_3Ch::PARAM_WAVES;
                ch = param / nWAVEFORMS;
                but = param - (ch * nWAVEFORMS);

                mymodule->m_Wave[ ch ].wavetype = but;
                mymodule->SetWaveLights();
            }

		    MomentarySwitch::onChange();
	    }
    };

    //-----------------------------------------------------
    // MynWaves_Knob
    //-----------------------------------------------------
    struct MynWaves_Knob : Yellow3_Med
    {
        Osc_3Ch *mymodule;
        int param;

        void onChange() override 
        {
            mymodule = (Osc_3Ch*)module;

            if( mymodule )
            {
                param = paramId - Osc_3Ch::PARAM_nWAVES;
                mymodule->m_nWaves[ param ] = (int)( value * (float)(MAX_nWAVES - 1) ); 
            }

		    RoundKnob::onChange();
	    }
    };

    //-----------------------------------------------------
    // MyEQHi_Knob
    //-----------------------------------------------------
    struct MyDetune_Knob : Yellow3_Med
    {
        Osc_3Ch *mymodule;
        int param;

        void onChange() override 
        {
            mymodule = (Osc_3Ch*)module;

            if( mymodule )
            {
                param = paramId - Osc_3Ch::PARAM_DETUNE;

                mymodule->m_DetuneIn[ param ] = value;
                mymodule->CalcDetune( param );
            }

		    RoundKnob::onChange();
	    }
    };

    //-----------------------------------------------------
    // MyEQHi_Knob
    //-----------------------------------------------------
    struct MySpread_Knob : Yellow3_Med
    {
        Osc_3Ch *mymodule;
        int param;

        void onChange() override 
        {
            mymodule = (Osc_3Ch*)module;

            if( mymodule )
            {
                param = paramId - Osc_3Ch::PARAM_SPREAD;

                mymodule->m_SpreadIn[ param ] = value;
                mymodule->CalcSpread( param );
            }

		    RoundKnob::onChange();
	    }
    };

    // Overrides 
	void    step() override;
    json_t* toJson() override;
    void    fromJson(json_t *rootJ) override;
    void    initialize() override;
    void    randomize() override;
    //void    reset() override;

    void    CalcSpread( int ch );
    void    CalcDetune( int ch );
    void    SetWaveLights( void );
    void    BuildWaves( void );
    void    ChangeFilterCutoff( int ch, float cutfreq );
    void    Filter( int ch, float *InL, float *InR );
    float   GetWave( int type, float phase );
    float   ProcessADR( int ch );
    void    GetAudio( int ch, float *pOutL, float *pOutR );
};

//-----------------------------------------------------
// Procedure:   
//
//-----------------------------------------------------
json_t *Osc_3Ch::toJson() 
{
    json_t *gatesJ;
	json_t *rootJ = json_object();

    // wavetypes
	gatesJ = json_array();

	for (int i = 0; i < nCHANNELS; i++)
    {
		json_t *gateJ = json_integer( m_Wave[ i ].wavetype );
		json_array_append_new( gatesJ, gateJ );
	}

	json_object_set_new( rootJ, "wavetypes", gatesJ );

	return rootJ;
}

//-----------------------------------------------------
// Procedure:   fromJson
//
//-----------------------------------------------------
void Osc_3Ch::fromJson(json_t *rootJ) 
{
    int i;
    json_t *StepsJ;

    // wave select
	StepsJ = json_object_get( rootJ, "wavetypes" );

	if (StepsJ) 
    {
		for ( i = 0; i < nCHANNELS; i++)
        {
			json_t *gateJ = json_array_get(StepsJ, i);

			if (gateJ)
				m_Wave[ i ].wavetype = json_integer_value( gateJ );
		}
	
    }

    // set up parameters
    for ( i = 0; i < nCHANNELS; i++)
    {
        m_nWaves[ i ] = (int)( params[ PARAM_nWAVES + i ].value * (float)(MAX_nWAVES - 1) );
               
        m_SpreadIn[ i ] = params[ PARAM_SPREAD + i ].value;
        CalcSpread( i );
        m_DetuneIn[ i ] = params[ PARAM_DETUNE + i ].value;
        CalcDetune( i );
    }

    SetWaveLights();
}

//-----------------------------------------------------
// Procedure:   Widget
//
//-----------------------------------------------------
Osc_3Ch_Widget::Osc_3Ch_Widget() 
{
    int ch, x, y, x2, y2, i;
	Osc_3Ch *module = new Osc_3Ch();
	setModule(module);
	box.size = Vec( 15*21, 380);

	{
		SVGPanel *panel = new SVGPanel();
		panel->box.size = box.size;
		panel->setBackground(SVG::load(assetPlugin(plugin, "res/OSC3Channel.svg")));
		addChild(panel);
	}

    //module->lg.Open("OSC3Channel.txt");

	addChild(createScrew<ScrewSilver>(Vec(15, 0)));
	addChild(createScrew<ScrewSilver>(Vec(box.size.x-30, 0)));
	addChild(createScrew<ScrewSilver>(Vec(15, 365))); 
	addChild(createScrew<ScrewSilver>(Vec(box.size.x-30, 365)));

    y = CHANNEL_Y;
    x = CHANNEL_X;

    for( ch = 0; ch < nCHANNELS; ch++ )
    {
        x = CHANNEL_X;

        // inputs
        addInput(createInput<MyPortInSmall>( Vec( x, y ), module, Osc_3Ch::IN_VOCT + ch ) );
        addInput(createInput<MyPortInSmall>( Vec( x, y + 43 ), module, Osc_3Ch::IN_TRIG + ch ) );

        x2 = x + 33;
        y2 = y + 52;

        for( i = 0; i < Osc_3Ch::nWAVEFORMS; i++ )
        {
            addParam(createParam<Osc_3Ch::MyWaveButton>( Vec( x2, y2 ), module, Osc_3Ch::PARAM_WAVES + ( ch * Osc_3Ch::nWAVEFORMS ) + i, 0.0, 1.0, 0.0 ) );
            addChild(createValueLight<SmallLight<YellowValueLight>>( Vec( x2 + 1, y2 + 2 ), &module->m_fLightWaveSelect[ ch ][ i ] ) );
            x2 += 16;
        }

        x2 = x + 24;
        y2 = y + 18;

        // params
        addParam(createParam<Yellow2_Small>( Vec( x2, y2 ), module, Osc_3Ch::PARAM_ATT + ch, 0.0, 1.0, 0.0 ) );

        x2 += 31;

        addParam(createParam<Yellow2_Small>( Vec( x2, y2 ), module, Osc_3Ch::PARAM_DELAY + ch, 0.0, 1.0, 0.0 ) );

        x2 += 31;

        addParam(createParam<Yellow2_Small>( Vec( x2, y2 ), module, Osc_3Ch::PARAM_REL + ch, 0.0, 1.0, 0.0 ) );

        // waves/detune/spread
        x2 = x + 149;
        y2 = y + 56;

        addParam(createParam<Osc_3Ch::MynWaves_Knob>( Vec( x + 129, y + 11 ), module, Osc_3Ch::PARAM_nWAVES + ch, 0.0, 1.0, 0.0 ) );
        addParam(createParam<Osc_3Ch::MyDetune_Knob>( Vec( x + 116, y + 48 ), module, Osc_3Ch::PARAM_DETUNE + ch, 0.0, 1.0, 0.0 ) );
        addParam(createParam<Osc_3Ch::MySpread_Knob>( Vec( x + 116 + 28, y + 48 ), module, Osc_3Ch::PARAM_SPREAD + ch, 0.0, 1.0, 0.0 ) );

        // inputs
        x2 = x + 178;
        y2 = y + 51;

        addInput(createInput<MyPortInSmall>( Vec( x2, y2 ), module, Osc_3Ch::IN_FILTER + ch ) );
        x2 += 36;
        addInput(createInput<MyPortInSmall>( Vec( x2, y2 ), module, Osc_3Ch::IN_REZ + ch ) );
        x2 += 40;
        addInput(createInput<MyPortInSmall>( Vec( x2, y2 ), module, Osc_3Ch::IN_LEVEL + ch ) );

        // filter
        y2 = y + 6;
        x2 = x + 167; 
        addParam(createParam<Green1_Big>( Vec( x2, y2 ), module, Osc_3Ch::PARAM_CUTOFF + ch, 0.0, 0.4, 0.0 ) );
        addParam(createParam<FilterSelectToggle>( Vec( x2 + 43, y2 + 2 ), module, Osc_3Ch::PARAM_FILTER_MODE + ch, 0.0, 3.0, 0.0 ) );
        addParam(createParam<Purp1_Med>( Vec( x2 + 46, y2 + 20 ), module, Osc_3Ch::PARAM_RES + ch, 0.0, 1.0, 0.0 ) );

        // main level
        addParam(createParam<Blue2_Med>( Vec( x2 + 76, y2 ), module, Osc_3Ch::PARAM_OUTLVL + ch, 0.0, 1.0, 0.0 ) );

        // outputs
        addOutput(createOutput<MyPortOutSmall>( Vec( x + 283, y + 4 ), module, Osc_3Ch::OUTPUT_AUDIO + (ch * 2) ) );
        addOutput(createOutput<MyPortOutSmall>( Vec( x + 283, y + 53 ), module, Osc_3Ch::OUTPUT_AUDIO + (ch * 2) + 1 ) );
        
        y += CHANNEL_H;
        module->m_nWaves[ ch ] = 0;
    }

    module->BuildWaves();
    module->SetWaveLights();
}

//-----------------------------------------------------
// Procedure:   initialize
//
//-----------------------------------------------------
void Osc_3Ch::initialize()
{
}

//-----------------------------------------------------
// Procedure:   randomize
//
//-----------------------------------------------------
void Osc_3Ch::randomize()
{
    int ch;

    for( ch = 0; ch < nCHANNELS; ch++ )
    {
        m_Wave[ ch ].wavetype = (int)( randomf() * (nWAVEFORMS-1) );
    }

    SetWaveLights();
}

//-----------------------------------------------------
// Procedure:   SetWaveLights
//
//-----------------------------------------------------
void Osc_3Ch::SetWaveLights( void )
{
    int ch;

    memset( m_fLightWaveSelect, 0, sizeof(m_fLightWaveSelect) );

    for( ch = 0; ch < nCHANNELS; ch++ )
    {
        m_fLightWaveSelect[ ch ][ m_Wave[ ch ].wavetype ] = 1.0;
    }
}

//-----------------------------------------------------
// Procedure:   initialize
//
//-----------------------------------------------------
#define DEG2RAD( x ) ( ( x ) * ( 3.14159f / 180.0f ) )
void Osc_3Ch::BuildWaves( void )
{
    int i;
    float finc, pos, val;

    finc = 360.0 / WAVE_BUFFER_LEN;
    pos = 0;

    // create sin wave
    for( i = 0; i < WAVE_BUFFER_LEN; i++ )
    {
        m_BufferWave[ WAVE_SIN ][ i ] = sin( DEG2RAD( pos ) );
        pos += finc;
    }

    // create sqr wave
    for( i = 0; i < WAVE_BUFFER_LEN; i++ )
    {
        if( i < WAVE_BUFFER_LEN / 2 )
            m_BufferWave[ WAVE_SQR ][ i ] = 1.0;
        else
            m_BufferWave[ WAVE_SQR ][ i ] = -1.0;
    }

    finc = 2.0 / (float)WAVE_BUFFER_LEN;
    val = 1.0;

    // create saw wave
    for( i = 0; i < WAVE_BUFFER_LEN; i++ )
    {
        m_BufferWave[ WAVE_SAW ][ i ] = val;

        val -= finc;
    }

    finc = 4 / (float)WAVE_BUFFER_LEN;
    val = 0;

    // create tri wave
    for( i = 0; i < WAVE_BUFFER_LEN; i++ )
    {
        m_BufferWave[ WAVE_TRI ][ i ] = val;

        if( i < WAVE_BUFFER_LEN / 4 )
            val += finc;
        else if( i < (WAVE_BUFFER_LEN / 4) * 3 )
            val -= finc;
        else if( i < WAVE_BUFFER_LEN  )
            val += finc;
    }
}

//-----------------------------------------------------
// Procedure:   GetAudio
//
//-----------------------------------------------------
float Osc_3Ch::GetWave( int type, float phase )
{
    float fval = 0.0;
    float ratio = (float)(WAVE_BUFFER_LEN-1) / gSampleRate;

    switch( type )
    {
    case WAVE_SIN:
    case WAVE_TRI:
    case WAVE_SQR:
    case WAVE_SAW:
        fval = m_BufferWave[ type ][ int( ( phase * ratio ) + 0.5 ) ];
        break;

    case WAVE_NOISE:
        fval = ( randomf() > 0.5 ) ? (randomf() * -1.0) : randomf();
        break;

    default:
        break;
    }

    return fval;
}

//-----------------------------------------------------
// Procedure:   ProcessADS
//
//-----------------------------------------------------
float Osc_3Ch::ProcessADR( int ch )
{
    ADR_STRUCT *padr;

    padr = &m_Wave[ ch ].adr_wave;

    // retrig the adsr
    if( padr->bTrig )
    {
        padr->state = ADR_ATTACK;

        padr->acount = 20 + (int)( params[ PARAM_ATT + ch ].value * 2.0 * gSampleRate );;
        padr->fainc  = 1.0 / (float)padr->acount;

        padr->dcount = 20 + (int)( params[ PARAM_DELAY + ch ].value * 4.0 * gSampleRate );

        padr->rcount = 20 + (int)( params[ PARAM_REL + ch ].value * 10.0 * gSampleRate );
        padr->frinc = 1.0 / (float)padr->rcount;

        //m_Wave[ ch ].phase = 0.0;
        padr->out = 0.0;
        padr->bTrig = false;
    }

    // process
    switch( padr->state )
    {
    case ADR_OFF:
        padr->out = 0.0;
        break;

    case ADR_ATTACK:
        padr->out += padr->fainc;
        if( --padr->acount <= 0 )
            padr->state = ADR_DELAY;
        break;

    case ADR_DELAY:
        padr->out = 1.0;
        if( --padr->dcount <= 0 )
            padr->state = ADR_RELEASE;
        break;

    case ADR_RELEASE:
        padr->out -= padr->frinc;
        if( --padr->rcount <= 0 )
        {
            padr->state = ADR_OFF;
            padr->out = 0.0;
        }
        break;
    }

    return clampf( padr->out, 0.0, 1.0 );
}

//-----------------------------------------------------
// Procedure:   ChangeFilterCutoff
//
//-----------------------------------------------------
void Osc_3Ch::ChangeFilterCutoff( int ch, float cutfreq )
{
    float fx, fx2, fx3, fx5, fx7;

    // clamp at 1.0 and 20/samplerate
    cutfreq = fmax(cutfreq, 20 / gSampleRate); 
    cutfreq = fmin(cutfreq, 1.0);

    // calculate eq rez freq
    fx = 3.141592 * (cutfreq * 0.026315789473684210526315789473684) * 2 * 3.141592; 
    fx2 = fx*fx;
    fx3 = fx2*fx; 
    fx5 = fx3*fx2; 
    fx7 = fx5*fx2;

    m_Wave[ ch ].f = 2.0 * (fx 
	    - (fx3 * 0.16666666666666666666666666666667) 
	    + (fx5 * 0.0083333333333333333333333333333333) 
	    - (fx7 * 0.0001984126984126984126984126984127));
}

//-----------------------------------------------------
// Procedure:   Filter
//
//-----------------------------------------------------
#define MULTI (0.33333333333333333333333333333333f)
void Osc_3Ch::Filter( int ch, float *InL, float *InR )
{
    OSC_PARAM_STRUCT *p;
    float rez, hp1; 
    float input[ 2 ], out[ 2 ], lowpass, bandpass, highpass;

    if( (int)params[ PARAM_FILTER_MODE + ch ].value == 0 )
        return;

    p = &m_Wave[ ch ];

    rez = 1.0 - params[ PARAM_RES + ch ].value;

    input[ 0 ] = *InL;
    input[ 1 ] = *InR;

    // do left and right channels
    for( int i = 0; i < 2; i++ )
    {
        input[ i ]  = input[ i ] + 0.000000001;

        p->lp1[ i ] = p->lp1[ i ] + p->f * p->bp1[ i ]; 
        hp1         = input[ i ] - p->lp1[ i ] - rez * p->bp1[ i ]; 
        p->bp1[ i ] = p->f * hp1 + p->bp1[ i ]; 
        lowpass     = p->lp1[ i ]; 
        highpass    = hp1; 
        bandpass    = p->bp1[ i ]; 

        p->lp1[ i ] = p->lp1[ i ] + p->f * p->bp1[ i ]; 
        hp1         = input[ i ] - p->lp1[ i ] - rez * p->bp1[ i ]; 
        p->bp1[ i ] = p->f * hp1 + p->bp1[ i ]; 
        lowpass     = lowpass  + p->lp1[ i ]; 
        highpass    = highpass + hp1; 
        bandpass    = bandpass + p->bp1[ i ]; 

        input[ i ]  = input[ i ] - 0.000000001;

        p->lp1[ i ] = p->lp1[ i ] + p->f * p->bp1[ i ]; 
        hp1         = input[ i ] - p->lp1[ i ] - rez * p->bp1[ i ]; 
        p->bp1[ i ] = p->f * hp1 + p->bp1[ i ]; 

        lowpass  = (lowpass  + p->lp1[ i ]) * MULTI; 
        highpass = (highpass + hp1) * MULTI; 
        bandpass = (bandpass + p->bp1[ i ]) * MULTI;

        switch( (int)params[ PARAM_FILTER_MODE + ch ].value )
        {
        case FILTER_LP:
            out[ i ] = lowpass;
            break;
        case FILTER_HP:
            out[ i ]  = highpass;
            break;
        case FILTER_BP:
            out[ i ]  = bandpass;
            break;
        default:
            break;
        }
    }

    *InL = out[ 0 ];
    *InR = out[ 1 ];
}

//-----------------------------------------------------
// Procedure:   CalcSpread
//
//-----------------------------------------------------
typedef struct
{
    float pan[ 2 ];
    float maxdetune;
}PAN_DETUNE;

PAN_DETUNE pandet[ 7 ][ 7 ] = 
{
    { { {1.0, 1.0}, 0.0 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 } },
    { { {1.0, 0.5}, 0.1 }, { {0.5, 1.0}, 0.2 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 } },
    { { {1.0, 0.5}, 0.3 }, { {1.0, 1.0}, 0.0 }, { {0.5, 1.0}, 0.2 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 } },
    { { {1.0, 0.3}, 0.4 }, { {1.0, 0.5}, 0.2 }, { {0.5, 1.0}, 0.2 }, { {0.3, 1.0}, 0.3 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 } },
    { { {1.0, 0.3}, 0.5 }, { {1.0, 0.5}, 0.4 }, { {1.0, 1.0}, 0.0 }, { {0.5, 1.0}, 0.3 }, { {0.3, 1.0}, 0.1 }, { {0.0, 0.0}, 0.0 }, { {0.0, 0.0}, 0.0 } },
    { { {1.0, 0.2}, 0.6 }, { {1.0, 0.3}, 0.4 }, { {1.0, 0.5}, 0.2 }, { {0.5, 1.0}, 0.3 }, { {0.3, 1.0}, 0.5 }, { {0.2, 1.0}, 0.8 }, { {0.0, 0.0}, 0.0 } },
    { { {1.0, 0.0}, 0.9 }, { {1.0, 0.2}, 0.7 }, { {1.0, 0.3}, 0.5 }, { {1.0, 1.0}, 0.0 }, { {0.3, 1.0}, 0.4 }, { {0.2, 1.0}, 0.8 }, { {0.0, 1.0}, 1.0 } },
};

void Osc_3Ch::CalcSpread( int ch )
{
    int used;  // number of waves being used by channel
    int wave;  // values for each individual wave

    // calculate pans for each possible number of waves being used
    for( used = 0; used < MAX_nWAVES; used++ )
    {
        for( wave = 0; wave <= used; wave++ )
        {
            m_Pan[ ch ][ used ][ wave ][ 0 ] =  ( 1.0 - m_SpreadIn[ ch ] ) + ( pandet[ used ][ wave ].pan[ 0 ] * m_SpreadIn[ ch ] );
            m_Pan[ ch ][ used ][ wave ][ 1 ] =  ( 1.0 - m_SpreadIn[ ch ] ) + ( pandet[ used ][ wave ].pan[ 1 ] * m_SpreadIn[ ch ] );
        }
    }
}

void Osc_3Ch::CalcDetune( int ch )
{
    int used;  // number of waves being used by channel
    int wave;  // values for each individual wave

    // calculate detunes for each possible number of waves being used
    for( used = 0; used < MAX_nWAVES; used++ )
    {
        for( wave = 0; wave <= used; wave++ )
            m_Detune[ ch ][ used ][ wave ] = pandet[ used ][ wave ].maxdetune * MAX_DETUNE * m_DetuneIn[ ch ];
    }
}

//-----------------------------------------------------
// Procedure:   GetAudio
//
//-----------------------------------------------------
void Osc_3Ch::GetAudio( int ch, float *pOutL, float *pOutR )
{
    float foutL = 0, foutR = 0, cutoff, adr;
    int i;

    for( i = 0; i <= m_nWaves[ ch ]; i++ )
    {
        foutL = GetWave( m_Wave[ ch ].wavetype, m_Wave[ ch ].phase[ i ] );
        foutR = foutL;

        foutL *= m_Pan[ ch ][ m_nWaves[ ch ] ][ i ][ 0 ];
        foutR *= m_Pan[ ch ][ m_nWaves[ ch ] ][ i ][ 1 ];

        // 38.8909 65.4064
        m_Wave[ ch ].phase[ i ] += 38.8909 * powf( 2.0, inputs[ IN_VOCT + ch ].value ) + m_Detune[ ch ][ m_nWaves[ ch ] ][ i ];

        if( m_Wave[ ch ].phase[ i ] >= gSampleRate )
            m_Wave[ ch ].phase[ i ] = m_Wave[ ch ].phase[ i ] - gSampleRate;

        *pOutL += foutL;
        *pOutR += foutR;
    }

    adr = ProcessADR( ch );

    *pOutL = *pOutL * adr;
    *pOutR = *pOutR * adr;

    cutoff = clampf( params[ PARAM_CUTOFF + ch ].value * ( inputs[ IN_FILTER + ch ].normalize( CV_MAX ) / CV_MAX ), 0.0, 1.0 );

    ChangeFilterCutoff( ch, cutoff );

    Filter( ch, pOutL, pOutR );
}

//-----------------------------------------------------
// Procedure:   step
//
//-----------------------------------------------------
void Osc_3Ch::step() 
{
    int ch;
    float outL, outR;

    // check for triggers
    for( ch = 0; ch < nCHANNELS; ch++ )
    {
        outL = 0.0;
        outR = 0.0;

	    if( inputs[ IN_TRIG + ch ].active ) 
        {
		    if( m_SchTrig[ ch ].process( inputs[ IN_TRIG + ch ].value ) )
            {
                m_Wave[ ch ].adr_wave.bTrig = true;
            }
	    }

        GetAudio( ch, &outL, &outR );

        outL = clampf( ( outL * AUDIO_MAX ) * params[ PARAM_OUTLVL + ch ].value * ( inputs[ IN_LEVEL + ch ].normalize( CV_MAX ) / CV_MAX ), -AUDIO_MAX, AUDIO_MAX );
        outR = clampf( ( outR * AUDIO_MAX ) * params[ PARAM_OUTLVL + ch ].value * ( inputs[ IN_LEVEL + ch ].normalize( CV_MAX ) / CV_MAX ), -AUDIO_MAX, AUDIO_MAX );

        outputs[ OUTPUT_AUDIO + (ch * 2 ) ].value = outL;
        outputs[ OUTPUT_AUDIO + (ch * 2 ) + 1 ].value = outR;
    }
}