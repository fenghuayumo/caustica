#include <render/passes/debug/Korgi.h>

#if KORGI_ENABLED

#include <WinSock2.h>
#include <mmsystem.h>
#include <unordered_map>

#ifdef _WIN32
#pragma comment(lib, "winmm")
#pragma comment(lib, "ws2_32")
#endif

// Help us out during dev by disabling optimisations so we can debug
//#pragma optimize("", off)

using namespace std;

namespace korgi
{

bool s_PageBit0 = false;
bool s_PageBit1 = false;
KORGI_TOGGLE( s_PageBit0, 0, FastForward )
KORGI_TOGGLE( s_PageBit1, 0, Rewind )

struct Controller
{
    void AddHook(unsigned char controlChannel, Knob* pParam)
    {
        knobs[controlChannel].push_back(pParam);
    }

    void AddHook(unsigned char controlChannel, Button* pParam)
    {
        buttons[controlChannel].push_back(pParam);
        setLedStatus(controlChannel, pParam);
    }

    bool init()
    {
        if (!OpenMidiDevice())
            return false;

        return true;
    }

    void shutdown()
    {
        CloseMidiDevice();
    }

    void update()
    {
        int currentPage = (s_PageBit0 ? 1 : 0) | (s_PageBit1 ? 2 : 0);
        if(currentPage != m_CurrentPage)
        {
            m_CurrentPage = currentPage;
            SetAllLeds();
        }
        else
        {
            // update the status of LEDs if the code has changed any of the button values
            for(const auto& it0 : buttons)
            {
                int cc = it0.first;
                for(Button* pButton : it0.second)
                {
                    if(((pButton->getPage() == -1) || (pButton->getPage() == m_CurrentPage))
                        && (pButton->getLedStatus() != pButton->getState()))
                    {
                        setLedStatus((unsigned char) cc, pButton);
                    }
                }
            }
        }
    }

    ~Controller()
    {
        shutdown();
    }

    static Controller* Get()
    {
        if (!s_pController)
        {
            s_pController = new Controller();
        }
        return s_pController;
    }

private:
    static void CALLBACK MidiInCallback(HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
    {
        if (wMsg != MIM_DATA)
            return;

        char controlChannel = (dwParam1 >> 8) & 0xff;
        char midiValue = (dwParam1 >> 16) & 0xff;

        s_pController->HandleMidiInput(controlChannel, midiValue);
    }


    void HandleMidiInput(unsigned char controlChannel, unsigned char midiValue)
    {
        auto button = buttons.find(controlChannel);
        auto knob = knobs.find(controlChannel);

        if (button != buttons.end())
        {
            for (auto b : button->second)
            {
                if((b->getPage() == -1) || (b->getPage() == m_CurrentPage))
                {
                    bool isPressed = (midiValue > 0);
                    switch(b->getMode())
                    {
                    case ButtonMode::Momentary:
                        // Set the value to the current state of the button
                        b->setState(isPressed);
                        setLedStatus(controlChannel, b);
                        break;
                    case ButtonMode::BoolToggle:
                    case ButtonMode::IntToggle:
                        if(isPressed)
                        {
                            // Toggle the button
                            if(b->getState())
                            {
                                // Turn off
                                b->setState(false);
                                setLedStatus(controlChannel, b);
                            }
                            else
                            {
                                // Turn on
                                b->setState(true);
                                setLedStatus(controlChannel, b);
                            }
                        }
                        break;
                    }
                }
            }
        }
        else if (knob != knobs.end())
        {
            float fvalue = (float)midiValue / 127.f;
            fvalue = max(0.f, min(1.f, fvalue));

            for (auto k : knob->second)
            {
                if((k->getPage() == -1) || (k->getPage() == m_CurrentPage))
                {
                    k->setValue(fvalue);
                }
            }
        }
    }

    bool OpenMidiDevice()
    {
        if (midiInOpen(&m_MidiInHandle, m_deviceIdx, (DWORD_PTR)MidiInCallback, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
        {
            return false;
        }

        midiInStart(m_MidiInHandle);

        // Try to open the nanoKONTROL2 as an output device
        uint32_t numOutputDevices = midiOutGetNumDevs();
        for(uint32_t i = 0; i < numOutputDevices; ++i)
        {
            MIDIOUTCAPS caps;
            midiOutGetDevCaps(i, &caps, sizeof(MIDIOUTCAPS));
            printf(caps.szPname);
            if(strncmp(caps.szPname, "nanoKONTROL2", strlen("nanoKONTROL2")) == 0)
            {
                if(midiOutOpen(&m_MidiOutHandle, i, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR)
                {
                    // Set the initial status of the LEDs
                    SetAllLeds();
                }
                break;
            }
        }

        MIDIINCAPS inCaps = {};
        MMRESULT res = midiInGetDevCaps((UINT_PTR)&m_deviceIdx, &inCaps, sizeof(MIDIINCAPS));

        // OWRIGHT : We Get MMSYSERR_BADDEVICEID, but it still works.
        return (res == MMSYSERR_NOERROR) || (res == MMSYSERR_BADDEVICEID);
    }

    void CloseMidiDevice()
    {
        if (m_MidiInHandle)
        {
            midiInClose(m_MidiInHandle);
            m_MidiInHandle = 0;
        }
    }

    void ClearAllLeds()
    {
        const unsigned char kFirstCcToClear = 32;
        const unsigned char kFinalCcToClear = 71;
        for (unsigned char cc = kFirstCcToClear; cc <= kFinalCcToClear; ++cc)
        {
            setLedStatus(cc, nullptr/*pButton*/);
        }
    }

    void SetAllLeds()
    {
        ClearAllLeds();
        for(const auto& it0 : buttons)
        {
            int cc = it0.first;
            for(Button* pButton : it0.second)
            {
                if((pButton->getPage() == -1) || (pButton->getPage() == m_CurrentPage))
                {
                    setLedStatus((unsigned char) cc, pButton);
                }
            }
        }
    }

    void setLedStatus(unsigned char controlChannel, Button* pButton)
    {
        if(m_MidiOutHandle)
        {
            union {
                DWORD dwData;
                BYTE bData[4];
            } u;
            const uint8_t kMidiChannel = 0;
            u.bData[0] = 0xb0/*control change*/ | kMidiChannel;  // MIDI status byte
            u.bData[1] = controlChannel;  // first MIDI data byte  : CC number
            u.bData[2] = (pButton && pButton->getState()) ? 127 : 0; // second MIDI data byte : Value
            u.bData[3] = 0;
            midiOutShortMsg(m_MidiOutHandle, u.dwData);
            if (pButton)
            {
                pButton->setLedStatus(pButton->getState());
            }
        }
    }

    //static const string device_name = "nanoKONTROL2";
    static Controller* s_pController;
    HMIDIIN  m_MidiInHandle = {};
    HMIDIOUT m_MidiOutHandle = {};
    int m_deviceIdx = 0;
    int m_CurrentPage = 0;

    // Maps indexed by control channel
    unordered_map<int, std::vector<Knob*>>   knobs;
    unordered_map<int, std::vector<Button*>> buttons;
};

Controller* korgi::Controller::s_pController = nullptr;

void init()
{
    Controller::Get()->init();
}
void shutdown()
{
    Controller::Get()->shutdown();
}
void update()
{
    Controller::Get()->update();
}

Button::Button(int page, Control controlChannel, ButtonMode mode, bool* pValue)
    : m_Mode(mode)
    , m_pValue(pValue ? (void*) pValue : (void*) &m_LocalState)
    , m_PreviousState(false)
    , m_LocalState(false)
    , m_OffValue((int)false)
    , m_OnValue((int)true)
    , m_Page(page)
{
    m_LedStatus = getState();
    Controller::Get()->AddHook((unsigned char)controlChannel, this);
}

Button::Button(int page, Control controlChannel, int* pValue, int offValue, int onValue)
    : m_Mode(ButtonMode::IntToggle)
    , m_pValue((void*)pValue)
    , m_PreviousState(false)
    , m_LocalState(false)
    , m_OffValue(offValue)
    , m_OnValue(onValue)
    , m_Page(page)
{
    m_LedStatus = getState();
    Controller::Get()->AddHook((unsigned char)controlChannel, this);
}

bool Button::getState() const
{
    if(getMode() == ButtonMode::IntToggle)
    {
        return *reinterpret_cast<const int*>(m_pValue) == m_OnValue;
    }
    return *reinterpret_cast<const bool*>(m_pValue);
}

void Button::setState(bool state)
{
    if (getMode() == ButtonMode::IntToggle)
    {
        *reinterpret_cast<int*>(m_pValue) = (state ? m_OnValue : m_OffValue);
        return;
    }
    *reinterpret_cast<bool*>(m_pValue) = state;
}

bool Button::wasMomentarilyPressed()
{
    bool retVal = false;
    const bool state = getState();
    if (state && !m_PreviousState)
    {
        retVal = true;
    }
    // clear the previous value, so this function only returns true once.
    m_PreviousState = state;
    return retVal;
}

Knob::Knob(int page, Control controlChannel, float* pValue, float mi, float ma)
    : m_pValue(pValue), m_MinValue(mi), m_MaxValue(ma), m_Page(page)
{
    Controller::Get()->AddHook((unsigned char)controlChannel, this);
}

} // namespace korgi

#endif // KORGI_ENABLED
