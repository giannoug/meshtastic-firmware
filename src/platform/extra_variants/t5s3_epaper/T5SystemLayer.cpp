#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5SystemLayer.h"

#include "BluetoothStatus.h"
#include "PowerFSM.h"
#include "PowerStatus.h"
#include "UptimeClock.h"
#include "gps/RTC.h"
#include "graphics/niche/InkHUD/InkHUD.h"
#include "graphics/niche/InkHUD/Persistence.h"
#include "graphics/niche/InkHUD/Tile.h"
#include "mesh/NodeDB.h"
#include "mesh/Throttle.h"
#include "modules/TextMessageModule.h"

#include <cstring>

using namespace NicheGraphics;

namespace
{
InkHUD::T5SystemLayer *instance = nullptr;

// Set on the firmware's low-voltage path, so the final image can say why the
// device stopped. There is no reason argument on the deep-sleep notification,
// and guessing from the voltage at shutdown would re-derive a threshold the
// firmware already owns.
bool batteryEmptyShutdown = false;

// Carry 508 x 928 and Console 928 x 508 are specified separately and
// independently (System UI 14). Neither is derived from the other, and neither
// reuses the Phase 2 nav cell arithmetic.
const InkHUD::T5SystemLayer::Geometry CARRY = {
    /* bannerTop */ 644,   /* bannerH */ 196,
    /* tagTop */ 658,      /* tagH */ 30,
    /* previewTop */ 698,  /* previewH */ 52, // 2 x 26
    /* previewLeft */ 16,  /* previewW */ 476,
    /* cardTop */ 392,
    /* bandTop */ 840,
    /* actionTop */ 760,   /* actionH */ 64,
    /* openLeft */ 16,     /* openW */ 230,
    /* dismissLeft */ 262, /* dismissW */ 230,
    /* hitTop */ 752,      /* hitH */ 80,
    /* hitLeft */ 8,       /* hitRight */ 500,
    /* hitSplit */ 254,
    /* pad */ 24,
};
const InkHUD::T5SystemLayer::Geometry CONSOLE = {
    /* bannerTop */ 308,
    /* bannerH */ 112,
    /* tagTop */ 320,
    /* tagH */ 30,
    /* previewTop */ 358,
    /* previewH */ 48, // 2 x 24
    /* previewLeft */ 16,
    /* previewW */ 580, // x 16-596: clear of the buttons
    /* cardTop */ 124,
    /* bandTop */ 420,
    /* actionTop */ 328,
    /* actionH */ 64,
    /* openLeft */ 620,
    /* openW */ 140,
    /* dismissLeft */ 776,
    /* dismissW */ 136,
    /* hitTop */ 316,
    /* hitH */ 92,
    /* hitLeft */ 612,
    /* hitRight */ 920,
    /* hitSplit */ 768,
    /* pad */ 16,
};

// 1970-01-01 was a Thursday, so the epoch day count starts 4 days into the week
const char *const WEEKDAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// The T5 foreground context the frozen suppression rules are written against
// (System UI 24). Messaging does not exist until Phase 4; until it does,
// nothing reports itself as a Messaging screen and the message-banner
// suppression below is simply never triggered.
bool foregroundIsMessaging()
{
    return false; // Phase 4: true for the T5 Messages list, a conversation, and
                  // the Console split
}

// A task layer owns the bottom of the screen and its own input, so banners are
// held rather than drawn over it (M5). The shared keyboard and the shared menu
// are the only task layers that exist today.
bool foregroundIsTask()
{
    for (const InkHUD::SystemApplet *sa : InkHUD::InkHUD::getInstance()->systemApplets) {
        if (!sa->handleInput || sa == instance)
            continue;
        if (sa->name &&
            (strcmp(sa->name, "Keyboard") == 0 || strcmp(sa->name, "Menu") == 0 || strcmp(sa->name, "AppSwitcher") == 0))
            return true;
    }
    return false;
}
} // namespace

InkHUD::T5SystemLayer::T5SystemLayer() : concurrency::OSThread("T5SystemLayer")
{
    // A full-screen tile, so every coordinate below is the frozen screen
    // coordinate, and we blank only the rect each surface actually occupies. That
    // is what keeps the snapshot underneath us intact, and what leaves the nav
    // visible and live under a banner.
    clearsOwnRegion = true;
    (new Tile)->assignApplet(this);
}

InkHUD::T5SystemLayer *InkHUD::T5SystemLayer::get()
{
    assert(instance);
    return instance;
}

InkHUD::T5SystemLayer *InkHUD::T5SystemLayer::getIfBegun()
{
    return instance;
}

void InkHUD::T5SystemLayer::begin()
{
    assert(!instance);
    instance = new T5SystemLayer;
    instance->name = "T5SystemLayer";
    instance->activate();

    // Last in the list: system applets render in registration order, so ours
    // draws over every other surface, and its input claim is checked after any
    // task layer's (System UI 04).
    InkHUD::getInstance()->systemApplets.push_back(instance);

    instance->getTile()->setRegion(0, 0, InkHUD::getInstance()->width(), InkHUD::getInstance()->height());

    instance->textMessageObserver.observe(textMessageModule);
    instance->bluetoothStatusObserver.observe(&bluetoothStatus->onNewStatus);

    instance->lastTickMs = Time::getMillis();
    instance->setIntervalFromNow(1000);
}

const InkHUD::T5SystemLayer::Geometry &InkHUD::T5SystemLayer::geometry() const
{
    return width() > height() ? CONSOLE : CARRY;
}

// ---------------------------------------------------------------------------------------------
// Arbitration
// ---------------------------------------------------------------------------------------------

bool InkHUD::T5SystemLayer::messageVisible() const
{
    if (terminal != Terminal::NONE || cardVisible != Card::NONE)
        return false; // Held behind the higher surface, timeout paused
    if (messages.empty())
        return false;
    return !foregroundIsMessaging() && !foregroundIsTask();
}

bool InkHUD::T5SystemLayer::infoVisible() const
{
    if (terminal != Terminal::NONE || cardVisible != Card::NONE || info == Info::NONE)
        return false;
    if (!messages.empty() && messageVisible())
        return false; // One banner slot: the message banner holds it (K8)
    return !foregroundIsTask();
}

// The lock band does not count: it replaces the nav, and the screen above it is
// free to redraw.
bool InkHUD::T5SystemLayer::coversContent() const
{
    return terminal != Terminal::NONE || cardVisible != Card::NONE || messageVisible() || infoVisible();
}

bool InkHUD::T5SystemLayer::bandVisible() const
{
    // Never over a terminal state (Lock and Power 04). Otherwise: the timeout
    // gate, or the user's own touch preference. Read from the gate flags, never
    // from the PowerFSM state (V12).
    if (terminal != Terminal::NONE)
        return false;
    return T5UiGate::snapshotHeld() || !t5TouchPreferenceEnabled();
}

int16_t InkHUD::T5SystemLayer::surfaceTop() const
{
    if (terminal != Terminal::NONE)
        return 0;

    const Geometry &g = geometry();
    int16_t top = -1;
    if (cardVisible != Card::NONE)
        top = g.cardTop;
    else if (messageVisible() || infoVisible())
        top = g.bannerTop;

    if (top < 0 && bandVisible())
        top = g.bandTop - 2; // Alone in the nav slot: include the rule above it
    return top;
}

// One request for the whole layer. Surfaces never request individually: a
// revealed surface and the one it replaces are a single redraw (System UI 18).
void InkHUD::T5SystemLayer::requestSurfaceUpdate()
{
    // full=false: the Renderer must not clear our tile. Each surface clears its
    // own rect in onRender.
    requestUpdate(EInk::UpdateTypes::UNSPECIFIED, false);
}

void InkHUD::T5SystemLayer::reconcile()
{
    const uint32_t now = Time::getMillis();

    // Drop surfaces which expired under a lifetime of their own. While gated this
    // changes state only; the wake reconcile runs before the wake frame, so an
    // expired surface is never drawn (D6b).
    if (info != Info::NONE && infoExpiresAtAwakeMs == 0 && infoDropAtMs != 0 && Throttle::deadlinePassedAt(now, infoDropAtMs)) {
        info = Info::NONE;
        infoDropAtMs = 0;
    }

    // Awake-time timers. awakeMs stops accumulating while gated, so these cannot
    // expire in the dark.
    if (!messages.empty() && messageExpiresAtAwakeMs != 0 && awakeMs >= messageExpiresAtAwakeMs)
        clearAggregate();
    if (info != Info::NONE && infoExpiresAtAwakeMs != 0 && awakeMs >= infoExpiresAtAwakeMs) {
        info = Info::NONE;
        infoExpiresAtAwakeMs = 0;
    }

    // Promote a held card once the slot is free (System UI 05: strictly by
    // priority, one redraw)
    if (cardVisible == Card::NONE && cardHeld != Card::NONE) {
        cardVisible = cardHeld;
        cardHeld = Card::NONE;
        disarm(); // G7: revealing a held surface counts as a new appearance
    }

    // Start the info banner's shown-timeout the first time it is actually
    // visible, and stop counting its unshown lifetime from that point.
    if (infoVisible() && infoExpiresAtAwakeMs == 0) {
        infoExpiresAtAwakeMs = awakeMs + INFO_SHOWN_MS;
        infoDropAtMs = 0;
    }

    const bool anySurface =
        terminal != Terminal::NONE || cardVisible != Card::NONE || messageVisible() || infoVisible() || bandVisible();

    // A terminal state replaces the frame: nothing else may draw or request.
    lockRendering = terminal != Terminal::NONE;
    lockRequests = lockRendering;

    // Drawn last in every frame, so a redraw of the screen underneath cannot
    // erase us.
    alwaysRender = anySurface;

    // Input is claimed only by surfaces that are targets. The lock band is not
    // one - with touch gated no touch arrives anyway, and while the user's own
    // TOUCH OFF is set the app below stays live and must keep the Home action.
    handleInput = terminal != Terminal::NONE || cardVisible != Card::NONE || messageVisible() || infoVisible();

    // bringToForeground() requests an update itself, which is exactly right for a
    // surface that has just appeared - and exactly wrong once per tick, so this
    // is a transition only.
    if (anySurface) {
        if (!isForeground())
            bringToForeground();
    } else if (isForeground())
        sendToBackground();
}

void InkHUD::T5SystemLayer::clearAggregate()
{
    messages = Aggregate();
    messageExpiresAtAwakeMs = 0;
    messagePending = false;
    memset(conversationIds, 0, sizeof(conversationIds));
}

void InkHUD::T5SystemLayer::noteConversation(uint32_t id)
{
    for (uint8_t i = 0; i < messages.conversations; i++) {
        if (conversationIds[i] == id)
            return;
    }
    if (messages.conversations < CONVERSATION_CAP)
        conversationIds[messages.conversations++] = id;
    else
        messages.overflowed = true;
}

// ---------------------------------------------------------------------------------------------
// Stale-touch gate (System UI G1-G8)
// ---------------------------------------------------------------------------------------------

// A surface is appearing, or has moved. Everything visible goes back behind the
// gate (G1, G6, G7).
void InkHUD::T5SystemLayer::disarm()
{
    armPending = true;
    renderedAtMs = 0;
    armedAtMs = 0;
}

// G1: armed only once (a) the refresh which drew the surface has finished, (b)
// no finger is on the panel, and (c) 300 ms have passed since (a). Called from
// runOnce, which ticks fast while this is outstanding.
void InkHUD::T5SystemLayer::tryArm()
{
    if (!armPending || renderedAtMs == 0)
        return; // Not drawn yet
    if (inkhud->isUpdating())
        return; // (a) the panel is still driving that refresh
    if (t5TouchIsDown())
        return; // (b) a finger is down: it must not be the one that activates this
                // surface

    armPending = false;
    armedAtMs = Time::stampMillis(); // (c) starts the 300 ms
}

bool InkHUD::T5SystemLayer::armed() const
{
    if (armPending || armedAtMs == 0 || !Throttle::hasElapsed(armedAtMs, ARM_MS))
        return false;

    // G2: a touch sequence belongs to the surface it started on. A contact that
    // began before this surface was armed never activates it, even if it is
    // released inside the surface's rect.
    const uint32_t downAt = t5TouchDownAtMs();
    return downAt == 0 || !Throttle::deadlinePassedAt(armedAtMs, downAt);
}

// ---------------------------------------------------------------------------------------------
// Timeout gate
// ---------------------------------------------------------------------------------------------

void InkHUD::T5SystemLayer::onTimeoutGateSet()
{
    // The stamp is captured once, here, and never recomputed: it says when the
    // snapshot was taken, not how long ago that was. Validity uses the same
    // source and threshold InkHUD already trusts for message timestamps - never a
    // plausible-looking epoch on its own (Lock and Power 04, V13).
    stampEpoch = getValidTime(RTCQuality::RTCQualityDevice, true);
    stampValid = stampEpoch != 0;

    disarm(); // Coordinates and surfaces change: every visible surface re-arms
    reconcile();
}

void InkHUD::T5SystemLayer::onTimeoutGateClear()
{
    stampValid = false;
    stampEpoch = 0;

    // Step 1 of the frozen wake sequence: apply logical state before anything is
    // drawn, so the wake frame is already correct and no second refresh is
    // needed. The caller then renders one complete frame as a normal UNSPECIFIED
    // update.
    reconcile();
    disarm();
}

void InkHUD::T5SystemLayer::onTouchPreferenceChanged()
{
    disarm();
    reconcile();
}

// ---------------------------------------------------------------------------------------------
// Periodic work
// ---------------------------------------------------------------------------------------------

int32_t InkHUD::T5SystemLayer::runOnce()
{
    const uint32_t now = Time::getMillis();
    const bool gated = T5UiGate::snapshotHeld();

    tryArm();

    // The awake clock. Frozen banner timeouts count awake time only, so it simply
    // stops here while the snapshot gate is set and resumes on wake.
    if (!gated)
        awakeMs += now - lastTickMs;
    lastTickMs = now;

    checkPower();

    const bool wasMessageVisible = messageVisible();
    const bool wasInfoVisible = infoVisible();
    const Card wasCard = cardVisible;

    reconcile();

    // A batched aggregate change becomes visible when its window closes. Channel
    // traffic is never the reason for a dark panel update, so while gated this
    // stays logical until a permitted layer redraw or the wake frame (System UI
    // 08, A4).
    if (messagePending && messageBatchUntilMs != 0 && Throttle::deadlinePassedAt(now, messageBatchUntilMs)) {
        messagePending = false;
        if (messageVisible() && (!gated || messages.hasDM))
            requestSurfaceUpdate();
    }

    // Something stopped being shown: the foreground screen has to come back where
    // it was covered.
    if ((wasMessageVisible && !messageVisible()) || (wasInfoVisible && !infoVisible()) ||
        (wasCard != Card::NONE && cardVisible != wasCard)) {
        if (!gated)
            inkhud->forceUpdate(EInk::UpdateTypes::UNSPECIFIED,
                                true); // renderAll: R1, exactly what the screen would draw now
    }

    // While a surface is waiting to arm, tick at the gate's own resolution rather
    // than rounding its 300 ms up to the next second.
    return (armPending || (armedAtMs != 0 && !Throttle::hasElapsed(armedAtMs, ARM_MS))) ? 50 : 1000;
}

// ---------------------------------------------------------------------------------------------
// Incoming messages (System UI 06)
// ---------------------------------------------------------------------------------------------

int InkHUD::T5SystemLayer::onReceiveTextMessage(const meshtastic_MeshPacket *p)
{
    if (isFromUs(p))
        return 0;

    const bool dm = !isBroadcast(p->to);
    const uint32_t sender = getFrom(p);
    const bool gated = T5UiGate::snapshotHeld();

    // Merge into the one aggregate slot. A DM always keeps the headline over
    // channel traffic.
    if (dm || !messages.hasDM) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(sender);
        if (node && nodeInfoLiteHasUser(node)) {
            messages.shortName = parseShortName(node);
            messages.longName = parse(node->long_name);
        } else {
            // No NodeDB entry: the node id in !hex form, exactly as frozen. Never an
            // invented name.
            char id[12];
            snprintf(id, sizeof(id), "!%08x", sender);
            messages.shortName = id;
            messages.longName = "unknown node";
        }
        messages.channelHeadline = !dm;
        messages.channel = p->channel;
        messages.timestamp = getValidTime(RTCQuality::RTCQualityDevice, true);
        if (p->decoded.payload.size)
            messages.preview = std::string((const char *)p->decoded.payload.bytes, p->decoded.payload.size);
    }

    messages.count++;
    messages.hasDM = messages.hasDM || dm;
    noteConversation(dm ? sender : (0x80000000u | p->channel));
    if (gated)
        messages.fromSleep = true;

    // Carry expires the banner after a minute of awake time; Console keeps it
    // until it is dealt with (decision 3). Either way the clock is awake time, so
    // sleeping does not expire it.
    messageExpiresAtAwakeMs = (width() > height()) ? 0 : awakeMs + MESSAGE_TIMEOUT_MS;

    reconcile();

    if (!messageVisible())
        return 0; // Held: suppressed inside Messaging (M4), or behind a task or a
                  // higher surface (M5)

    // First arrival draws immediately; later ones inside the window are batched
    // to one redraw per 10 s.
    const bool windowOpen = messageBatchUntilMs != 0 && !Throttle::deadlinePassed(messageBatchUntilMs);
    if (windowOpen) {
        messagePending = true;
        return 0;
    }
    messageBatchUntilMs = Time::timerEndsAtMillis(MESSAGE_BATCH_MS);
    messagePending = false;

    // A DM may draw over the snapshot as an approved dark layer: no front light,
    // not a user-input wake, the timeout gate and the light preference untouched.
    // Channel traffic may not - it has already updated the aggregate above and
    // will first be seen in the wake frame (decision 4, A4).
    if (gated && !dm)
        return 0;

    disarm(); // A surface is appearing: it must not activate under a finger
              // already down (G1)
    requestSurfaceUpdate();
    return 0;
}

// ---------------------------------------------------------------------------------------------
// BLE pairing (System UI 09)
// ---------------------------------------------------------------------------------------------

int InkHUD::T5SystemLayer::onBluetoothStatusUpdate(const meshtastic::Status *status)
{
    assert(status->getStatusType() == STATUS_TYPE_BLUETOOTH);
    const auto *bt = static_cast<const meshtastic::BluetoothStatus *>(status);

    if (bt->getConnectionState() == meshtastic::BluetoothStatus::ConnectionState::PAIRING) {
        passkey = bt->getPasskey();

        // Critical battery outranks pairing, so an open critical card keeps the
        // slot and pairing waits.
        if (cardVisible == Card::CRITICAL_BATTERY)
            cardHeld = Card::PAIRING;
        else if (cardVisible != Card::PAIRING) {
            cardVisible = Card::PAIRING;
            disarm();
        }
        reconcile();
        requestSurfaceUpdate(); // Approved over the snapshot: the base stays
                                // frozen, the light stays off
        return 0;
    }

    // Pairing ended. Success is silent; the card simply closes. A pairing that
    // stops without connecting leaves the frozen info banner behind.
    const bool wasPairing = cardVisible == Card::PAIRING || cardHeld == Card::PAIRING;
    if (!wasPairing)
        return 0;

    const bool connected = bt->getConnectionState() == meshtastic::BluetoothStatus::ConnectionState::CONNECTED;
    if (cardVisible == Card::PAIRING)
        cardVisible = Card::NONE;
    if (cardHeld == Card::PAIRING)
        cardHeld = Card::NONE;
    passkey.clear();

    if (!connected && info == Info::NONE) {
        info = Info::PAIRING_FAILED;
        infoExpiresAtAwakeMs = 0;
        infoDropAtMs = Time::timerEndsAtMillis(INFO_UNSEEN_MS);
    }

    reconcile();
    // The card is gone: the screen beneath has to come back exactly as it would
    // draw now (R1). While gated the snapshot cannot be rebuilt, so this waits
    // for the wake frame.
    if (!T5UiGate::snapshotHeld())
        inkhud->forceUpdate(EInk::UpdateTypes::UNSPECIFIED, true);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Battery (System UI 10)
// ---------------------------------------------------------------------------------------------

void InkHUD::T5SystemLayer::checkPower()
{
    if (Throttle::isWithinTimespanMs(lastPowerCheckMs, POWER_CHECK_MS))
        return; // Evaluated at most once a minute (System UI 10)
    lastPowerCheckMs = Time::stampMillis();

    if (!powerStatus || !powerStatus->getHasBattery())
        return; // No battery: no thresholds to cross, and no number to trust

    const bool usb = powerStatus->getHasUSB();
    const uint8_t pct = powerStatus->getBatteryChargePercent();

    // USB clears both surfaces and re-arms both crossings. No charging banners,
    // ever.
    if (usb) {
        pendingLowSamples = 0;
        pendingCriticalSamples = 0;
        lowArmed = true;
        criticalArmed = true;
        bool changed = false;
        if (info == Info::LOW_BATTERY) {
            info = Info::NONE;
            infoExpiresAtAwakeMs = 0;
            infoDropAtMs = 0;
            changed = true;
        }
        if (cardVisible == Card::CRITICAL_BATTERY) {
            cardVisible = Card::NONE;
            changed = true;
        }
        if (cardHeld == Card::CRITICAL_BATTERY) {
            cardHeld = Card::NONE;
            changed = true;
        }
        if (changed) {
            reconcile();
            if (!T5UiGate::snapshotHeld())
                inkhud->forceUpdate(EInk::UpdateTypes::UNSPECIFIED, true);
        }
        return;
    }

    // Re-arm above the hysteresis point, so one discharge produces one surface.
    if (pct >= LOW_REARM_PCT)
        lowArmed = true;
    if (pct >= CRITICAL_REARM_PCT)
        criticalArmed = true;

    // A crossing must hold for two consecutive checks: the percentage is a
    // straight-line voltage estimate, so a single sample at the boundary means
    // nothing.
    pendingCriticalSamples = (pct <= CRITICAL_ENTER_PCT) ? pendingCriticalSamples + 1 : 0;
    pendingLowSamples = (pct <= LOW_ENTER_PCT) ? pendingLowSamples + 1 : 0;

    if (criticalArmed && pendingCriticalSamples >= 2) {
        criticalArmed = false;
        lowArmed = false; // Already past low: it must not fire on the way down
        if (info == Info::LOW_BATTERY) {
            info = Info::NONE; // Escalation: the banner becomes the card in one redraw
            infoExpiresAtAwakeMs = 0;
            infoDropAtMs = 0;
        }
        if (cardVisible != Card::CRITICAL_BATTERY) {
            if (cardVisible != Card::NONE)
                cardHeld = cardVisible; // Pairing waits behind it
            cardVisible = Card::CRITICAL_BATTERY;
            disarm();
        }
        reconcile();
        requestSurfaceUpdate(); // The one battery surface allowed to draw over the
                                // snapshot
        return;
    }

    if (lowArmed && pendingLowSamples >= 2) {
        lowArmed = false;
        if (info != Info::LOW_BATTERY) {
            info = Info::LOW_BATTERY;
            infoExpiresAtAwakeMs = 0;
            infoDropAtMs = Time::timerEndsAtMillis(INFO_UNSEEN_MS);
        }
        reconcile();
        // Low battery never wakes or redraws a gated panel: it is shown in the wake
        // frame.
        if (!T5UiGate::snapshotHeld() && infoVisible()) {
            disarm();
            requestSurfaceUpdate();
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Terminal states (System UI 11)
// ---------------------------------------------------------------------------------------------

void InkHUD::T5SystemLayer::noteBatteryEmptyShutdown()
{
    batteryEmptyShutdown = true;
}

// The shared shutdown path already waits for any update in flight, then
// performs one blocking FULL after every applet has been notified. Setting the
// state here means that existing refresh draws the final image: one FULL, no
// intermediate frame, nothing left to replace it (System UI 11).
void InkHUD::T5SystemLayer::onShutdown()
{
    showTerminal(batteryEmptyShutdown ? Terminal::BATTERY_EMPTY : Terminal::SWITCHED_OFF);
}

// Reboot has no forced update of its own, so this one is ours. Blocking, so it
// completes before the firmware pulls the rug.
void InkHUD::T5SystemLayer::onReboot()
{
    showTerminal(Terminal::RESTARTING);
    inkhud->forceUpdate(EInk::UpdateTypes::FULL, true, false);
}

void InkHUD::T5SystemLayer::showTerminal(Terminal t)
{
    terminal = t;
    // A terminal state replaces the snapshot rather than layering over it: held
    // surfaces are discarded.
    cardVisible = Card::NONE;
    cardHeld = Card::NONE;
    info = Info::NONE;
    clearAggregate();
    reconcile();
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

bool InkHUD::T5SystemLayer::onTouchPoint(uint16_t x, uint16_t y, bool longPress)
{
    if (terminal != Terminal::NONE)
        return true; // All input ignored from the moment the final image is drawn

    const Geometry &g = geometry();

    if (cardVisible != Card::NONE) {
        // The card captures the whole screen. Outside its single button nothing
        // happens, and nothing reaches the inert screen above it.
        if (!longPress && armed() && y >= g.bandTop)
            onExitShort(); // The band holds the card's one safe verb
        return true;
    }

    const bool message = messageVisible();
    if (!message && !infoVisible())
        return false;

    // The banner rect wins hit testing over anything it intersects, including the
    // still-visible part of a row it only half covers. The nav below it stays
    // live.
    if (y < g.bannerTop || y >= g.bandTop)
        return false;

    if (longPress || !armed())
        return true; // Consumed, but a stale or held touch never activates a target
                     // (G1, G2)

    const bool hitRow = y >= g.hitTop && y < g.hitTop + g.hitH;
    if (!hitRow || x < g.hitLeft || x >= g.hitRight)
        return true; // The banner body is deliberately not a target

    const bool dismiss = x >= g.hitSplit;
    if (!message && !dismiss)
        return true; // An info banner has no OPEN; its left half is not a target

    if (dismiss) {
        // DISMISS clears this banner only. It never marks anything read: unread
        // belongs to Messaging.
        if (message)
            clearAggregate();
        else {
            info = Info::NONE;
            infoExpiresAtAwakeMs = 0;
            infoDropAtMs = 0;
        }
    } else {
        // OPEN. Native Messaging arrives in Phase 4; until then the aggregate is
        // consumed exactly as it would be by entering the conversation, and nothing
        // is routed into the shared applets.
        clearAggregate();
    }

    reconcile();
    disarm(); // Whatever is revealed next is a new appearance (G7)
    inkhud->forceUpdate(EInk::UpdateTypes::UNSPECIFIED,
                        true); // One request for both rects
    return true;
}

// The T5 Home action reached us, so a temporary layer is showing: peel exactly
// one (System UI 17). Home never confirms anything, and the next press goes
// Home.
void InkHUD::T5SystemLayer::onExitShort()
{
    if (terminal != Terminal::NONE)
        return;

    if (cardVisible != Card::NONE) {
        // The card's safe verb: HIDE for pairing (the stack exposes no way to
        // reject), OK for the rest.
        if (cardVisible == Card::PAIRING)
            passkey.clear();
        cardVisible = Card::NONE;
    } else if (messageVisible()) {
        clearAggregate();
    } else if (infoVisible()) {
        info = Info::NONE;
        infoExpiresAtAwakeMs = 0;
        infoDropAtMs = 0;
    } else {
        return;
    }

    reconcile();
    disarm();
    inkhud->forceUpdate(EInk::UpdateTypes::UNSPECIFIED, true);
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

// Everything we own clears its own rect, and nothing else: the Renderer never
// clears our tile, so the frozen snapshot and the live foreground beneath us
// both survive a surface redraw (System UI 18, R3). A banner in particular
// stops at the nav, which stays visible and tappable underneath it.
void InkHUD::T5SystemLayer::clearSurface(int16_t top, int16_t bottom)
{
    fillRect(0, top, width(), bottom - top, WHITE);
}

void InkHUD::T5SystemLayer::onRender(bool full)
{
    (void)full;

    // Note that the surface has been drawn. Arming waits for the refresh itself
    // to finish, and for the panel to be finger-free, both of which runOnce
    // checks (G1).
    if (armPending)
        renderedAtMs = Time::stampMillis();

    if (terminal != Terminal::NONE) {
        drawTerminal();
        return;
    }

    if (surfaceTop() < 0)
        return;

    const Geometry &g = geometry();
    if (cardVisible != Card::NONE) {
        clearSurface(g.cardTop, height()); // The card owns the nav slot too
        drawCard();
    } else if (messageVisible() || infoVisible()) {
        clearSurface(g.bannerTop, g.bandTop); // Stops at the nav
        drawBanner();
    }

    if (bandVisible())
        drawBand(); // Clears the nav slot itself, over a card's button band if
                    // there was one
}

// The lock band: a STATUS surface in the nav slot. Not a target, no timeout. It
// names the one press that makes the device respond again, and nothing else
// (Lock and Power 04).
void InkHUD::T5SystemLayer::drawBand()
{
    const Geometry &g = geometry();
    const bool console = width() > height();
    const bool asleep = T5UiGate::snapshotHeld();

    // ASLEEP wins when both are true: the band always states the next press,
    // never both.
    const char *chip = asleep ? "ASLEEP" : "TOUCH OFF";
    const char *line1 =
        console ? (asleep ? "Press the side button to wake" : "Press the side button to turn touch on") : "Press the side button";
    const char *line2 = asleep ? "to wake" : "to turn touch on";

    fillRect(0, g.bandTop - 2, width(), height() - g.bandTop + 2, WHITE);
    fillRect(0, g.bandTop - 2, width(), 2,
             BLACK); // The rule that replaces the nav's own

    setFont(fontSmall);
    const int16_t lh = fontSmall.lineHeight();
    const int16_t bandMid = g.bandTop + (height() - g.bandTop) / 2;

    // The chip: outlined for the timeout, inverted for the user's own setting.
    const int16_t chipW = getTextWidth(chip) + 2 * 12 + 4;
    const int16_t chipH = lh + 2 * 6 + 4;
    const int16_t chipTop = bandMid - chipH / 2;
    if (asleep) {
        drawRect(T5Applet::MARGIN, chipTop, chipW, chipH, BLACK);
        drawRect(T5Applet::MARGIN + 1, chipTop + 1, chipW - 2, chipH - 2, BLACK);
        printThick(T5Applet::MARGIN + chipW / 2, bandMid, chip, 2, 1);
    } else {
        fillRect(T5Applet::MARGIN, chipTop, chipW, chipH, BLACK);
        setTextColor(WHITE);
        printThick(T5Applet::MARGIN + chipW / 2, bandMid, chip, 2, 1);
        setTextColor(BLACK);
    }

    // The stamp, right-aligned. Captured once when the gate was set, so it does
    // not tick while asleep.
    int16_t textRight = width() - T5Applet::MARGIN;
    if (asleep) {
        std::string second;
        const std::string first = stampText(!console, &second);
        if (second.empty())
            printAt(width() - T5Applet::MARGIN, bandMid, first, RIGHT, MIDDLE);
        else {
            printAt(width() - T5Applet::MARGIN, bandMid - 2, first, RIGHT, BOTTOM);
            printBoldRight(width() - T5Applet::MARGIN, bandMid + 2, second);
        }
        const std::string widest = getTextWidth(first) > getTextWidth(second) ? first : second;
        textRight = width() - T5Applet::MARGIN - getTextWidth(widest) - T5Applet::MARGIN;
    }

    // The instruction, between the chip and the stamp.
    const int16_t textLeft = T5Applet::MARGIN + chipW + 16;
    setCrop(textLeft, g.bandTop, textRight > textLeft ? textRight - textLeft : 0, height() - g.bandTop);
    if (console)
        printBold(textLeft, bandMid - lh / 2, line1);
    else {
        printBold(textLeft, bandMid - lh, line1);
        printAt(textLeft, bandMid, line2, LEFT, TOP);
    }
    resetCrop();
}

// "Since Sat 14:32" once the clock is trusted, "Time not set" before that.
// Never elapsed time, never uptime, never seconds (Lock and Power 04).
std::string InkHUD::T5SystemLayer::stampText(bool twoLine, std::string *second) const
{
    second->clear();
    if (!stampValid)
        return "Time not set";

    const uint32_t hms = stampEpoch % SEC_PER_DAY;
    const uint32_t hour = hms / SEC_PER_HOUR;
    const uint32_t minute = (hms % SEC_PER_HOUR) / SEC_PER_MIN;

    char clock[12];
    if (config.display.use_12h_clock)
        snprintf(clock, sizeof(clock), "%u:%02u %s", (hour % 12 == 0) ? 12u : (unsigned)(hour % 12), (unsigned)minute,
                 hour > 11 ? "PM" : "AM");
    else
        snprintf(clock, sizeof(clock), "%02u:%02u", (unsigned)hour, (unsigned)minute);

    const std::string weekday = WEEKDAYS[((stampEpoch / SEC_PER_DAY) + 4) % 7];
    if (twoLine) {
        *second = clock;
        return "Since " + weekday;
    }
    return "Since " + weekday + " " + clock;
}

// One shell for both banner kinds: an action banner has OPEN, an info banner
// does not. One geometry, one redraw region, one place for DISMISS (System UI
// 02).
void InkHUD::T5SystemLayer::drawBanner()
{
    const Geometry &g = geometry();
    const bool console = width() > height();
    const bool message = messageVisible();

    fillRect(0, g.bannerTop, width(), 4, BLACK); // Heavy top rule

    const int16_t lh = fontSmall.lineHeight();

    // Tag, inverted. Kind, or a count when several arrived.
    std::string tag, who, preview, timeText;
    if (message) {
        const bool several = messages.conversations > 1;
        char buf[24];
        if (messages.fromSleep)
            tag = "WHILE ASLEEP";
        else if (several) {
            snprintf(buf, sizeof(buf), "%u NEW", (unsigned)messages.count);
            tag = buf;
        } else {
            if (messages.channelHeadline)
                snprintf(buf, sizeof(buf), "CH %u", (unsigned)messages.channel);
            else
                snprintf(buf, sizeof(buf), "DIRECT");
            tag = buf;
            if (messages.count > 1) {
                snprintf(buf, sizeof(buf), " %s %u", T5Applet::DASH, (unsigned)messages.count);
                tag += buf;
            }
        }

        if (several || messages.fromSleep) {
            char n[48];
            snprintf(n, sizeof(n), "%u new %s %u conversations", (unsigned)messages.count, T5Applet::DASH,
                     (unsigned)(messages.overflowed ? CONVERSATION_CAP : messages.conversations));
            who = messages.fromSleep ? n : (to_string(messages.conversations) + " conversations");
            if (!messages.fromSleep)
                who += std::string(" ") + T5Applet::DASH + " newest " + messages.shortName;
            preview = messages.shortName + ": " + messages.preview;
        } else {
            who = T5Applet::join(messages.shortName, messages.longName);
            preview = messages.preview;
        }
        timeText = messages.timestamp ? getTimeString(messages.timestamp) : "";
    } else if (info == Info::LOW_BATTERY) {
        tag = "BATTERY";
        char buf[40];
        snprintf(buf, sizeof(buf), "Low battery %s %u %%", T5Applet::DASH,
                 (unsigned)(powerStatus ? powerStatus->getBatteryChargePercent() : 0));
        who = buf;
        preview = "Charge soon. This shows once until the device is charged.";
    } else {
        tag = "BLUETOOTH";
        who = "Pairing not completed";
        preview = "Nothing was paired. Try again from the app.";
    }

    // Tag chip and sender share one line; the time sits at the right edge.
    setFont(fontSmall);
    const int16_t tagW = getTextWidth(tag) + 2 * 10 + 4;
    const int16_t tagMid = g.tagTop + g.tagH / 2;
    fillRect(T5Applet::MARGIN, g.tagTop, tagW, g.tagH, BLACK);
    setTextColor(WHITE);
    printThick(T5Applet::MARGIN + tagW / 2, tagMid, tag, 2, 1);
    setTextColor(BLACK);

    int16_t whoRight = width() - T5Applet::MARGIN;
    if (!timeText.empty()) {
        printAt(width() - T5Applet::MARGIN, tagMid, timeText, RIGHT, MIDDLE);
        whoRight = width() - T5Applet::MARGIN - getTextWidth(timeText) - T5Applet::MARGIN;
    }

    // The sender line is one line and is cut, never wrapped: the short name comes
    // first, so it survives.
    const int16_t whoLeft = T5Applet::MARGIN + tagW + 14;
    setCrop(whoLeft, g.tagTop, whoRight > whoLeft ? whoRight - whoLeft : 0, g.tagH);
    printBold(whoLeft, tagMid - lh / 2, who);
    resetCrop();

    // The preview is hard-limited to the frozen two-line box: the banner never
    // grows.
    setCrop(g.previewLeft, g.previewTop, g.previewW, g.previewH);
    printWrapped(g.previewLeft, g.previewTop, g.previewW, preview);
    resetCrop();

    // Actions. An info banner keeps DISMISS in the same place, so the hand learns
    // one position.
    if (message)
        drawBannerButton(g.openLeft, g.openW, messages.conversations > 1 ? "MESSAGES" : "OPEN");
    drawBannerButton(g.dismissLeft, g.dismissW, "DISMISS");
}

void InkHUD::T5SystemLayer::drawBannerButton(int16_t left, int16_t w, const char *label)
{
    const Geometry &g = geometry();
    setFont(fontSmall);
    drawRect(left, g.actionTop, w, g.actionH, BLACK);
    drawRect(left + 1, g.actionTop + 1, w - 2, g.actionH - 2, BLACK);
    printThick(left + w / 2, g.actionTop + g.actionH / 2, label, 2, 1);
}

// The card: heavy top rule, inverted event header, body, and a button band in
// the nav position. The screen above stays visible but inert (System UI 02,
// 14).
void InkHUD::T5SystemLayer::drawCard()
{
    const Geometry &g = geometry();
    const bool console = width() > height();
    const bool pairing = cardVisible == Card::PAIRING;

    fillRect(0, g.cardTop, width(), 4, BLACK);

    // Inverted event header: source and state on the left, a right-hand value.
    const int16_t headerTop = g.cardTop + 4;
    constexpr int16_t HEADER_H = 48;
    fillRect(0, headerTop, width(), HEADER_H, BLACK);
    setFont(fontSmall);
    setTextColor(WHITE);
    printThick(T5Applet::MARGIN + getTextWidth(pairing ? "BLUETOOTH \x97 PAIRING" : "BATTERY \x97 CRITICAL") / 2,
               headerTop + HEADER_H / 2, pairing ? "BLUETOOTH \x97 PAIRING" : "BATTERY \x97 CRITICAL", 2, 1);
    std::string right;
    if (pairing)
        right = getTimeString();
    else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u %%", (unsigned)(powerStatus ? powerStatus->getBatteryChargePercent() : 0));
        right = buf;
    }
    if (!right.empty())
        printAt(width() - T5Applet::MARGIN, headerTop + HEADER_H / 2, right, RIGHT, MIDDLE);
    setTextColor(BLACK);

    int16_t y = headerTop + HEADER_H + g.pad;
    const int16_t bodyW = width() - 2 * g.pad;

    if (pairing) {
        setFont(fontMedium);
        printBold(g.pad, y, console ? "Pairing request \x97 type this PIN on the phone" : "Pairing request");
        y += fontMedium.lineHeight() + (console ? 8 : 12);

        if (!console) {
            setFont(fontSmall);
            printWrapped(g.pad, y, bodyW, "Type this PIN on the phone that is pairing.");
            y += fontSmall.lineHeight() + 16;
        }

        // The PIN, grouped 3 + 3. fontLarge is the largest face loaded on T5, so it
        // stands in for the frozen 72 / 60 px type; thickening carries the
        // emphasis.
        setFont(fontLarge);
        const std::string pin = passkey.size() >= 6 ? passkey.substr(0, 3) + " " + passkey.substr(3) : passkey;
        printThick(width() / 2, y + fontLarge.lineHeight() / 2, pin, 3, 2);
        y += fontLarge.lineHeight() + (console ? 8 : 16);

        setFont(fontSmall);
        printWrapped(g.pad, y, bodyW,
                     console ? "Closes by itself when pairing finishes, or after 30 seconds."
                             : "This closes by itself when pairing finishes, or after 30 "
                               "seconds.");
        y += fontSmall.lineHeight() + 12;

        // The stack reports no peer name, so the row says so rather than inventing
        // one.
        if (!console)
            drawCardRow(y, "DEVICE", "NOT REPORTED");
    } else {
        setFont(fontMedium);
        printBold(g.pad, y, "Battery almost empty");
        y += fontMedium.lineHeight() + (console ? 8 : 12);

        setFont(fontSmall);
        const char *body = console ? "Turns itself off at 2 %. Connect USB now."
                                   : "The device turns itself off at 2 % to "
                                     "protect the battery. Connect USB now to "
                                     "keep it running.";
        printWrapped(g.pad, y, bodyW, body);
        y += (int16_t)getWrappedTextHeight(g.pad, bodyW, body) + 16;

        char buf[32];
        snprintf(buf, sizeof(buf), "%u %% \x97 %.2f V", (unsigned)(powerStatus ? powerStatus->getBatteryChargePercent() : 0),
                 powerStatus ? powerStatus->getBatteryVoltageMv() / 1000.0 : 0.0);
        drawCardRow(y, "BATTERY", buf);
    }

    // The button band sits in the nav position - unless the lock band has taken
    // that slot, in which case the buttons return in the wake or touch-on frame
    // (Lock and Power 04).
    if (!bandVisible())
        drawButtonBand(nullptr, pairing ? "HIDE" : "OK");
}

void InkHUD::T5SystemLayer::drawCardRow(int16_t top, const char *label, const std::string &value)
{
    const Geometry &g = geometry();
    setFont(fontSmall);
    printAt(g.pad, top, label, LEFT, TOP);
    printBoldRight(width() - g.pad, top, value);
}

// One button fills the band; two split it 1 : 1.3 with the verb on the right.
void InkHUD::T5SystemLayer::drawButtonBand(const char *left, const char *right)
{
    const Geometry &g = geometry();
    const int16_t top = g.bandTop;
    const int16_t h = height() - top;

    fillRect(0, top - 2, width(), 2, BLACK);
    setFont(fontSmall);

    if (!left) {
        printThick(width() / 2, top + h / 2, right, 2, 1);
        return;
    }

    const int16_t split = width() * 10 / 23; // 1 : 1.3
    printAt(split / 2, top + h / 2, left, CENTER, MIDDLE);
    fillRect(split, top, width() - split, h, BLACK);
    setTextColor(WHITE);
    printThick(split + (width() - split) / 2, top + h / 2, right, 2, 1);
    setTextColor(BLACK);
}

// The frozen full-screen template: title, one big line, one explanation,
// optional detail rows. No nav, no buttons, no band, and all input ignored
// (System UI 11, Menu C13).
void InkHUD::T5SystemLayer::drawTerminal()
{
    fillRect(0, 0, width(), height(), WHITE);

    const bool console = width() > height();
    const int16_t headerH = console ? CONSOLE_HEADER_H : CARRY_HEADER_H;
    const int16_t pad = console ? 16 : 24;

    const char *title = "";
    const char *big = "";
    std::string explain;
    switch (terminal) {
    case Terminal::RESTARTING:
        title = "RESTARTING";
        big = "Back in a few seconds";
        explain = "Settings are saved first. Rejoin your mesh on the same region "
                  "and preset.";
        break;
    case Terminal::SWITCHED_OFF:
        title = "SWITCHED OFF";
        big = "Press the power button to start";
        explain = "Turned off at " + getTimeString() + ". No messages are received while it is off.";
        break;
    case Terminal::BATTERY_EMPTY:
        title = "BATTERY EMPTY";
        big = "Charge to start";
        explain = "Turned off at " + getTimeString() + " to protect the battery. Connect USB, then press the power button.";
        break;
    default:
        return;
    }

    // Header: the title only. No back target, no status, no battery.
    setFont(fontSmall);
    printThick(T5Applet::MARGIN + getTextWidth(title) / 2, headerH / 2, title, 2, 1);
    fillRect(0, headerH, width(), 2, BLACK);

    int16_t y = headerH + 2 + (console ? 24 : 48);
    setFont(fontLarge);
    setCrop(pad, y, width() - 2 * pad, fontLarge.lineHeight() * 2);
    printWrapped(pad, y, width() - 2 * pad, big);
    y += (int16_t)getWrappedTextHeight(pad, width() - 2 * pad, big);
    resetCrop();

    y += console ? 12 : 20;
    setFont(fontSmall);
    printWrapped(pad, y, width() - 2 * pad, explain);
    y += (int16_t)getWrappedTextHeight(pad, width() - 2 * pad, explain) + (console ? 16 : 28);

    // DETAILS: only facts that cannot go stale while the image sits on the panel.
    meshtastic_NodeInfoLite *us = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (y + 3 * fontSmall.lineHeight() > height())
        return;

    fillRect(pad, y, width() - 2 * pad, 2, BLACK);
    y += 12;
    setFont(fontSmall);
    printAt(pad, y, "THIS DEVICE", LEFT, TOP);
    y += fontSmall.lineHeight() + 8;

    if (us && nodeInfoLiteHasUser(us))
        drawTerminalRow(y, "NODE", T5Applet::join(parseShortName(us), parse(us->long_name)));
    y += fontSmall.lineHeight() + 6;

    if (terminal == Terminal::SWITCHED_OFF && powerStatus && powerStatus->getHasBattery()) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u %%", (unsigned)powerStatus->getBatteryChargePercent());
        drawTerminalRow(y, "BATTERY AT SHUTDOWN", buf);
    }
}

void InkHUD::T5SystemLayer::drawTerminalRow(int16_t top, const char *label, const std::string &value)
{
    const int16_t pad = (width() > height()) ? 16 : 24;
    setFont(fontSmall);
    printAt(pad, top, label, LEFT, TOP);
    printBoldRight(width() - pad, top, value);
}

// Top-aligned bold, right edge given. printThick centres, so the caller's edge
// is converted here once.
void InkHUD::T5SystemLayer::printBold(int16_t x, int16_t y, const std::string &text)
{
    printThick(x + getTextWidth(text) / 2, y + getFont().lineHeight() / 2, text, 2, 1);
}

void InkHUD::T5SystemLayer::printBoldRight(int16_t right, int16_t y, const std::string &text)
{
    printThick(right - getTextWidth(text) / 2, y + getFont().lineHeight() / 2, text, 2, 1);
}

#endif
