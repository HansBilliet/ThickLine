#define _CRT_SECURE_NO_WARNINGS

#include <Core/CoreAll.h>
#include <Fusion/FusionAll.h>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <string>
#include <cstdlib>      // std::getenv
#include <filesystem>   // C++17
#include <fstream>
#include <system_error>

using namespace adsk::core;
using namespace adsk::fusion;

Ptr<Application> _app;
Ptr<UserInterface> _ui;

// IDs for the input fields
static const char* kGraphic = "tl_graphic";

static const char* kSeparator1 = "tl_sep1";
static const char* kSeparator2 = "tl_sep2";

static const char* kGroupA = "tl_groupA";
static const char* kGroupB = "tl_groupB";

static const char* kWidthId = "tl_width";

static const char* kSelPointAId = "tl_selPointA";
static const char* kLeadAId = "tl_leadA";
static const char* kFeatATypeId = "tl_featA_type";
static const char* kFeatAWidthId = "tl_featA_width";
static const char* kFeatALengthId = "tl_featA_length";

static const char* kSelPointBId = "tl_selPointB";
static const char* kLeadBId = "tl_leadB";
static const char* kFeatBTypeId = "tl_featB_type";
static const char* kFeatBWidthId = "tl_featB_width";
static const char* kFeatBLengthId = "tl_featB_length";

static const char* kErrorBox = "tl_errorBox";

// small numeric thresholds used everywhere
constexpr double kEpsCoincident = 1e-12; // point equality / normalization safety
constexpr double kEpsSketchLen = 1e-9;  // geometry construction guards

// Default settings (structure)
struct ThickLineSettings {
    double width_cm = 0.2;
	std::string featAType = "None";
    double leadA_cm = 0;
    double featAL_cm = 0.5;
    double featAW_cm = 0.5;
	std::string featBType = "None";
    double leadB_cm = 0;
    double featBL_cm = 0.5;
    double featBW_cm = 0.5;
};

// Get path to application data directory for this add-in
inline std::filesystem::path appDataDir()
{
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA"); // e.g. C:\Users\You\AppData\Roaming
    std::filesystem::path base = appdata ? appdata : "";
    return base / "Autodesk" / "Fusion" / "API" / "ThickLine";
#else
    const char* home = std::getenv("HOME");       // e.g. /Users/you
    std::filesystem::path base = home ? home : "";
    return base / "Library" / "Application Support" / "Autodesk" / "Fusion" / "API" / "ThickLine";
#endif
}

// Get path to settings.ini file
inline std::filesystem::path settingsPath()
{
    return appDataDir() / "settings.ini";
}

// Save settings to INI file
inline bool saveSettingsIni(const ThickLineSettings& s)
{
    std::error_code ec;
    std::filesystem::create_directories(appDataDir(), ec);

    std::ofstream f(settingsPath(), std::ios::trunc);
    if (!f) return false;

    f << "width_cm=" << s.width_cm << "\n";

    f << "featAType=" << s.featAType << "\n";
    f << "leadA_cm=" << s.leadA_cm << "\n";
    f << "featAL_cm=" << s.featAL_cm << "\n";
    f << "featAW_cm=" << s.featAW_cm << "\n";
    
    f << "featBType=" << s.featBType << "\n";
    f << "leadB_cm=" << s.leadB_cm << "\n";
    f << "featBL_cm=" << s.featBL_cm << "\n";
    f << "featBW_cm=" << s.featBW_cm << "\n";

    return true;
}

// Load settings from INI file
inline ThickLineSettings loadSettingsIni()
{
    ThickLineSettings s; // defaults
    std::ifstream f(settingsPath());
	if (!f) return s; // file not found, return defaults

    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        try {
            if (key == "featAType")      s.featAType = value;
            else if (key == "featBType") s.featBType = value;
            else
            {
                double v = std::stod(value);
                if (key == "width_cm")  s.width_cm = v;
                else if (key == "leadA_cm")  s.leadA_cm = v;
                else if (key == "leadB_cm")  s.leadB_cm = v;
                else if (key == "featAL_cm") s.featAL_cm = v;
                else if (key == "featAW_cm") s.featAW_cm = v;
                else if (key == "featBL_cm") s.featBL_cm = v;
                else if (key == "featBW_cm") s.featBW_cm = v;
            }
        }
        catch (...) {
            // ignore bad numbers
        }
    }
    return s;
}

// Helper: log message to Fusion console
inline void LogFusion(const std::string& s)
{
    if (_app) _app->log(s.c_str());
}

// Helper: manage error message box state
static struct ErrState
{
    bool lastValid = true;
    std::string lastMsg;
} g_ErrState;

// Update error message box visibility and content
static void syncErrorBox(const Ptr<CommandInputs>& inputs, bool valid, const std::string& msg)
{
    Ptr<TextBoxCommandInput> errBox = inputs->itemById(kErrorBox)->cast<TextBoxCommandInput>();
    if (!errBox)
        return;

    // Only update when something changed.
    const bool visNow = errBox->isVisible();
    const bool visWant = !valid;

    if (visNow != visWant)
    {
        errBox->isVisible(visWant);
    }

    // We don’t rely on a getter for formattedText; compare with cached lastMsg.
    if (!valid && g_ErrState.lastMsg != msg)
    {
		std::string fullMsg = "<font color='#d32f2f'>Error: " + msg + "</font>";
        errBox->formattedText(fullMsg.c_str());
    }

    // Update cache
    g_ErrState.lastValid = valid;
    g_ErrState.lastMsg = msg;
}

// Helper: add common selection filters for points
inline void addPointSelectionFilters(const Ptr<adsk::core::SelectionCommandInput>& sel)
{
    sel->addSelectionFilter("SketchPoints");
    sel->addSelectionFilter("ConstructionPoints");
    sel->addSelectionFilter("Vertices");
    sel->setSelectionLimits(0, 1);
}

// Helper: enable/disable Feature Width/Length based on dropdown selection
inline void updateFeatureInputs(const Ptr<CommandInputs>& inputs, const char* ddId, const char* wId, const char* lId)
{
    Ptr<DropDownCommandInput> dd = inputs->itemById(ddId)->cast<DropDownCommandInput>();
    Ptr<ValueCommandInput> w = inputs->itemById(wId)->cast<ValueCommandInput>();
    Ptr<ValueCommandInput> l = inputs->itemById(lId)->cast<ValueCommandInput>();

    if (!dd || !w || !l)
        return;

    Ptr<ListItem> sel = dd->selectedItem();
    bool isNone = !sel || sel->index() == 0; // index 0 == "None"

    if (w->isEnabled() == isNone) w->isEnabled(!isNone);
    if (l->isEnabled() == isNone) l->isEnabled(!isNone);
}

// Helper: get the 3D world point from a selected entity (SketchPoint, ConstructionPoint, or Vertex)
inline Ptr<Point3D> worldPointFromEntity(const Ptr<Base>& ent)
{
    if (!ent) return nullptr;

    Ptr<SketchPoint> sp = ent->cast<SketchPoint>();
    if (sp)
        return sp->worldGeometry();  // Sketch points: world coords

    Ptr<ConstructionPoint> cp = ent->cast<ConstructionPoint>();
    if (cp)
        return cp->geometry();       // Construction points: world coords

    Ptr<BRepVertex> v = ent->cast<BRepVertex>();
    if (v)
        return v->geometry();         // Vertices: world coords

    return nullptr;
}

// Helper: get the active sketch (if any)
inline Ptr<Sketch> getActiveSketch()
{
    Ptr<Sketch> sketch = nullptr;
    if (_app) {
        Ptr<Base> editObj = _app->activeEditObject();
        if (editObj)
            sketch = editObj->cast<Sketch>();
    }
    return sketch;
}

// 2D vector and some operations (in sketch space)
struct V2 { double x, y; };
inline V2 v2(double x, double y) { V2 v{ x, y }; return v; }
inline V2 vadd(const V2& a, const V2& b) { return v2(a.x + b.x, a.y + b.y); }
inline V2 vsub(const V2& a, const V2& b) { return v2(a.x - b.x, a.y - b.y); }
inline V2 vscale(const V2& a, double s) { return v2(a.x * s, a.y * s); }
inline double vlen(const V2& a) { return std::sqrt(a.x * a.x + a.y * a.y); }
inline double vdot(const V2& a, const V2& b) { return a.x * b.x + a.y * b.y; }
inline V2 vperp_ccw(const V2& a) { return v2(-a.y, a.x); } // 90deg CCW

inline Ptr<Point3D> P2(const V2& s) { return Point3D::create(s.x, s.y, 0.0); }

// Parameter bundle (structure)
struct ThickLineParams {
    // context
    Ptr<Sketch> sketch{ nullptr };

	// x and y coordinates of the two end points (in sketch space)
    V2 A{ };
    V2 B{ };

    // sizes (cm)
    double widthCm{ 0 };
    double leadACm{ 0 };
    double leadBCm{ 0 };

	// Feature A
	std::string featAType{ "None" };
    double featAWCm{ 0 };
    double featALCm{ 0 };

	// Feature B
    std::string featBType{ "None" };
    double featBWCm{ 0 };
    double featBLCm{ 0 };

    // Direction vectors
	double L{ 0 }; // length from A to B
	V2 Ldir{ };   // normalized direction from A to B
	V2 Wdir{ };   // normalized perpendicular to Ldir (90deg CCW)

    // Extended points of the line
	V2 Aext{ }; // extended A point (with leadA)
	V2 Bext{ }; // extended B point (with leadB)

	// Feature base points (along line)
	V2 Abase{ }; // base of Feature A (along line)
	V2 Bbase{ }; // base of Feature B (along line)
};

// Extract parameters from the command inputs
bool extractParams(const Ptr<CommandInputs>& inputs, ThickLineParams& P, std::string& err)
{
    // Sketch
    P.sketch = getActiveSketch();
    if (!P.sketch)
    {
        err = "Please edit a sketch before running this command.";
        return false;
    }

    // read inputs (cm)
    Ptr<ValueCommandInput> widthIn = inputs->itemById(kWidthId)->cast<ValueCommandInput>();
    Ptr<ValueCommandInput> leadAIn = inputs->itemById(kLeadAId)->cast<ValueCommandInput>();
    Ptr<ValueCommandInput> leadBIn = inputs->itemById(kLeadBId)->cast<ValueCommandInput>();
    P.widthCm = widthIn ? widthIn->value() : 0.0;
    P.leadACm = leadAIn ? leadAIn->value() : 0.0;
    P.leadBCm = leadBIn ? leadBIn->value() : 0.0;

    // read feature types
    Ptr<DropDownCommandInput> ddA = inputs->itemById(kFeatATypeId)->cast<DropDownCommandInput>();
    Ptr<DropDownCommandInput> ddB = inputs->itemById(kFeatBTypeId)->cast<DropDownCommandInput>();
    P.featAType = (ddA && ddA->selectedItem()) ? std::string(ddA->selectedItem()->name()) : "None";
    P.featBType = (ddB && ddB->selectedItem()) ? std::string(ddB->selectedItem()->name()) : "None";

	// read feature sizes (cm)
    Ptr<ValueCommandInput> featAWIn = inputs->itemById(kFeatAWidthId)->cast<ValueCommandInput>();
    Ptr<ValueCommandInput> featALIn = inputs->itemById(kFeatALengthId)->cast<ValueCommandInput>();
    Ptr<ValueCommandInput> featBWIn = inputs->itemById(kFeatBWidthId)->cast<ValueCommandInput>();
    Ptr<ValueCommandInput> featBLIn = inputs->itemById(kFeatBLengthId)->cast<ValueCommandInput>();
    P.featAWCm = (P.featAType != "None" && featAWIn) ? featAWIn->value() : 0.0;
    P.featALCm = (P.featAType != "None" && featALIn) ? featALIn->value() : 0.0;
    P.featBWCm = (P.featBType != "None" && featBWIn) ? featBWIn->value() : 0.0;
    P.featBLCm = (P.featBType != "None" && featBLIn) ? featBLIn->value() : 0.0;

    // Get selected points and convert from world coordinates to sketch coordinates
    Ptr<SelectionCommandInput> selA = inputs->itemById(kSelPointAId)->cast<SelectionCommandInput>();
    if (!selA || selA->selectionCount() == 0)
    { 
        err = "Select point or entity for A.";
        return false;
    }
    Ptr<SelectionCommandInput> selB = inputs->itemById(kSelPointBId)->cast<SelectionCommandInput>();
    if (!selB || selB->selectionCount() == 0)
    {
        err = "Select point or entity for B.";
        return false;
    }
    Ptr<Base> entA = selA->selection(0)->entity();
    Ptr<Base> entB = selB->selection(0)->entity();
    Ptr<Point3D> pA3 = worldPointFromEntity(entA);
    Ptr<Point3D> pB3 = worldPointFromEntity(entB);
    if (!pA3 || !pB3)
    {
        err = !pA3 ? "Could not read geometry for selection A. Please select a SketchPoint, ConstructionPoint, or Vertex."
                   : "Could not read geometry for selection B. Please select a SketchPoint, ConstructionPoint, or Vertex.";
        return false;
    }
    Ptr<Point3D> sA = P.sketch->modelToSketchSpace(pA3);
    Ptr<Point3D> sB = P.sketch->modelToSketchSpace(pB3);
    P.A = v2(sA->x(), sA->y());
    P.B = v2(sB->x(), sB->y());

    // distance between 2 selected points
    V2 diff = vsub(P.B, P.A);

    // Normalize direction vectors
    P.L = vlen(diff);
    if (P.L <= kEpsCoincident)
    { // <- early guard
        err = "Points A and B are coincident or too close together.";
        return false;
    }
    P.Ldir = vscale(diff, 1.0 / P.L);
	P.Wdir = vperp_ccw(P.Ldir);

    // Final endpoints after leads (tips where features end)
    P.Aext = vadd(P.A, vscale(P.Ldir, -P.leadACm)); // A tip
    P.Bext = vadd(P.B, vscale(P.Ldir, P.leadBCm)); // B tip

    // Feature bases pulled inward from tips by their own lengths
    P.Abase = vadd(P.Aext, vscale(P.Ldir, +P.featALCm)); // from A tip inward
    P.Bbase = vadd(P.Bext, vscale(P.Ldir, -P.featBLCm)); // from B tip inward

    return true;
}

// Validate parameters for geometric consistency
bool validateParams(const ThickLineParams& P, std::string& err)
{
	// width > 0
    if (P.widthCm <= 0)
    {
        err = "Width of line must be > 0.";
        return false;
    }

    // start and end points must not be coincident
    if (P.L <= kEpsCoincident)
    {
        err = "Points A and B are coincident or too close together.";
        return false;
    }

	// Check feature widths and lengths
    if (P.featAType != "None")
    {
        if (P.featAWCm < P.widthCm)
        {
            err = "Feature A width must be >= line width.";
            return false;
        }
        if (P.featALCm <= 0)
        {
            err = "Feature A length must be > 0.";
            return false;
		}
    }
    if (P.featBType != "None")
    {
        if (P.featBWCm < P.widthCm)
        {
            err = "Feature B width must be >= line width.";
            return false;
        }
        if (P.featBLCm <= 0)
        {
            err = "Feature B length must be > 0.";
            return false;
        }
    }

	// Main segment between feature bases
    V2 seg = vsub(P.Bbase, P.Abase);
    // Signed length along the intended direction.
    double segLenSigned = vdot(seg, P.Ldir);
    if (segLenSigned <= kEpsSketchLen) {
		err = "Leads and/or feature lengths consume the segment. Reduce leads/features or move A and B further apart.";
        return false;
    }

    return true;
}

// draw rectangle given 3 corners (in sketch space)
inline void drawThreePointRect(const Ptr<Sketch>& sk, const V2& p0, const V2& p1, const V2& p3)
{
    if (!sk)
        return;

    Ptr<SketchLines> lines = sk->sketchCurves()->sketchLines();
    Ptr<SketchLineList> rect = lines->addThreePointRectangle(P2(p0), P2(p1), P2(p3));

	rect->item(0)->isFixed(true);
	rect->item(1)->isFixed(true);
	rect->item(2)->isFixed(true);
	rect->item(3)->isFixed(true);
}

// draw triangle given 3 corners (in sketch space)
inline void drawTriangle(const Ptr<Sketch>& sk, const V2& a, const V2& b, const V2& c)
{
    if (!sk)
        return;

    Ptr<SketchLines> lines = sk->sketchCurves()->sketchLines();
    Ptr<SketchLine> l1 = lines->addByTwoPoints(P2(a), P2(b));
    Ptr<SketchLine> l2 = lines->addByTwoPoints(P2(b), P2(c));
    Ptr<SketchLine> l3 = lines->addByTwoPoints(P2(c), P2(a));

	l1->isFixed(true);
	l2->isFixed(true);
	l3->isFixed(true);
}

// Debug: dump all inputs
//inline void DumpInputs(const Ptr<CommandInputs>& ins, std::string_view tag)
//{
//    if (!ins) return;
//    LogFusion(std::string("[DumpInputs] ") + std::string(tag));
//    for (size_t i = 0; i < ins->count(); ++i) {
//        auto ci = ins->item(i);
//        if (!ci) continue;
//        LogFusion(std::string("  id='") + ci->id() + "'  type=" + ci->objectType());
//    }
//}

class ThickLineInputChangedEventHandler : public InputChangedEventHandler
{
public:
    void notify(const Ptr<InputChangedEventArgs>& eventArgs) override
    {
        Ptr<CommandInputs> inputs = eventArgs->inputs();
        Ptr<CommandInput> changed = eventArgs->input();
        if (!inputs || !changed)
            return;

        if (changed->id() == kSelPointAId)
        {
			Ptr<SelectionCommandInput> selA = changed->cast<SelectionCommandInput>();
            if (selA && selA->selectionCount() == 1)
            {
                // User just finished picking point A -> set focus to point B
                Ptr<CommandInputs> allInputs = inputs->command()->commandInputs(); // inputs only contains inputs of group A - need access to group B
                if (!allInputs)
                    return;
                Ptr<SelectionCommandInput> selB = allInputs->itemById(kSelPointBId)->cast<SelectionCommandInput>();
                if (selB)
                    selB->hasFocus(true);
            }
        }

        if (changed->id() == kFeatATypeId)
            updateFeatureInputs(inputs, kFeatATypeId, kFeatAWidthId, kFeatALengthId);

        if (changed->id() == kFeatBTypeId)
            updateFeatureInputs(inputs, kFeatBTypeId, kFeatBWidthId, kFeatBLengthId);

        if (changed->id() == kWidthId)
        {
            Ptr<ValueCommandInput> widthIn = inputs->itemById(kWidthId)->cast<ValueCommandInput>();
            double widthVal = widthIn ? widthIn->value() : 0.0;

            Ptr<ValueCommandInput> aW = inputs->itemById(kFeatAWidthId)->cast<ValueCommandInput>();
            Ptr<ValueCommandInput> bW = inputs->itemById(kFeatBWidthId)->cast<ValueCommandInput>();
            if (aW) aW->minimumValue(widthVal);
            if (bW) bW->minimumValue(widthVal);
        }
    }
} _thickLineInputChangedHandler;

class ThickLineValidateInputsEventHandler : public ValidateInputsEventHandler
{
public:
    void notify(const Ptr<ValidateInputsEventArgs>& eventArgs) override
    {
        Ptr<CommandInputs> inputs = eventArgs->inputs();
        if (!inputs)
            return;

		// Extract and validate parameters
		ThickLineParams P;
		std::string err;
		bool ok = extractParams(inputs, P, err) && validateParams(P, err);

		syncErrorBox(inputs, ok, err);

		eventArgs->areInputsValid(ok);
    }
} _thickLineValidateInputsHandler;

class ThickLineCommandEventHandler : public CommandEventHandler
{
public:
    void notify(const Ptr<CommandEventArgs>& eventArgs) override
    {
        Ptr<adsk::core::Command> cmd = eventArgs->command();
        Ptr<adsk::core::CommandInputs> inputs = cmd ? cmd->commandInputs() : nullptr;
        if (!cmd || !inputs)
            return;

        // Extract and validate parameters
        ThickLineParams P;
        std::string err;
        if (!extractParams(inputs, P, err) || !validateParams(P, err))
        {
            LogFusion("[ThickLine] Command failed: " + err + "\n");
            return;
		}

		// Half width vector
        V2 wHalf = vscale(P.Wdir, P.widthCm * 0.5);

        // --- main rectangle spans Abase <-> Bbase (skip if inverted/zero) ---
        V2 seg = vsub(P.Bbase, P.Abase);

        V2 Aplus = vadd(P.Abase, wHalf);
        V2 Aminus = vsub(P.Abase, wHalf);
        V2 Bplus = vadd(P.Bbase, wHalf);
        V2 Bminus = vsub(P.Bbase, wHalf);

		drawThreePointRect(P.sketch, Aplus, Bplus, Aminus); // ensures corners are closed

        // --- feature at A (tip fixed at Aext, depth = aLuse) ---
        if (P.featAType == "Arrow") {
            V2 aSide = vscale(P.Wdir, P.featAWCm * 0.5);
            V2 baseL = vadd(P.Abase, aSide);
            V2 baseR = vadd(P.Abase, vscale(aSide, -1.0));
            drawTriangle(P.sketch, baseL, P.Aext, baseR);
        }
        else if (P.featAType == "T") {
            V2 aSide = vscale(P.Wdir, P.featAWCm * 0.5);
            V2 aL0 = vadd(P.Abase, aSide);
            V2 aR0 = vadd(P.Abase, vscale(aSide, -1.0));
            V2 aL1 = vadd(aL0, vscale(P.Ldir, -P.featALCm)); // toward Aext
            V2 aR1 = vadd(aR0, vscale(P.Ldir, -P.featALCm));
			drawThreePointRect(P.sketch, aL0, aL1, aR0); // ensure corners are closed
        }

        // --- feature at B (tip fixed at Bext, depth = bLuse) ---
        if (P.featBType == "Arrow") {
            V2 bSide = vscale(P.Wdir, P.featBWCm * 0.5);
            V2 baseL = vadd(P.Bbase, bSide);
            V2 baseR = vadd(P.Bbase, vscale(bSide, -1.0));
            drawTriangle(P.sketch, baseL, P.Bext, baseR);
        }
        else if (P.featBType == "T") {
            V2 bSide = vscale(P.Wdir, P.featBWCm * 0.5);
            V2 bL0 = vadd(P.Bbase, bSide);
            V2 bR0 = vadd(P.Bbase, vscale(bSide, -1.0));
            V2 bL1 = vadd(bL0, vscale(P.Ldir, +P.featBLCm)); // toward Bext
            V2 bR1 = vadd(bR0, vscale(P.Ldir, +P.featBLCm));
			drawThreePointRect(P.sketch, bL0, bL1, bR0); // ensure corners are closed
        }

		ThickLineSettings S;
		S.width_cm = P.widthCm;
		S.leadA_cm = P.leadACm;
		S.featAType = P.featAType;
		S.featAL_cm = P.featALCm;
		S.featAW_cm = P.featAWCm;
        S.leadB_cm = P.leadBCm;
        S.featBType = P.featBType;
        S.featBL_cm = P.featBLCm;
		S.featBW_cm = P.featBWCm;
        saveSettingsIni(S); // save current settings

		LogFusion("[ThickLine] Settings saved to: " + settingsPath().string());
    }
} _thickLineCommandHandler;

class ThickLineCommandCreatedEventHandler : public CommandCreatedEventHandler
{
public:
    void notify(const Ptr<CommandCreatedEventArgs>& eventArgs) override
    {
		// Load settings from INI file (or use default values)
        ThickLineSettings S = loadSettingsIni();

        // Get the command from the event arguments.
		Ptr<Command> cmd = eventArgs->command();
        if (!cmd)
            return;

        // Add command inputs here if needed.
        Ptr<CommandInputs> inputs = cmd->commandInputs();
        if (!inputs)
            return;

        // Graphic
        Ptr<ImageCommandInput> img = inputs->addImageCommandInput(kGraphic, "", "Resources/Graphic200.png");
        img->isFullWidth(true); // make it stretch across the dialog

        // Separator under image
        inputs->addSeparatorCommandInput(kSeparator1);

        // ---- Width (global) ----
        Ptr<ValueCommandInput> widthInput = inputs->addValueInput(kWidthId, "Width", "mm", ValueInput::createByReal(S.width_cm));
		widthInput->minimumValue(0.0);

        // Separator under image
        inputs->addSeparatorCommandInput(kSeparator2);

        // Group A & B to mirror UI layout
        Ptr<GroupCommandInput> grpA = inputs->addGroupCommandInput(kGroupA, "Point A");
        grpA->isExpanded(true);
        Ptr<CommandInputs> giA = grpA->children();

        Ptr<GroupCommandInput> grpB = inputs->addGroupCommandInput(kGroupB, "Point B");
        grpB->isExpanded(true);
        Ptr<CommandInputs> giB = grpB->children();

        // ---- Point A block ----
        {
            // Select Point A
            Ptr<SelectionCommandInput> selA = giA->addSelectionInput(kSelPointAId, "Select Point A", "Pick the start point (A)");
            addPointSelectionFilters(selA);

            // Lead A
            Ptr<ValueCommandInput> leadA = giA->addValueInput(kLeadAId, "Lead A", "mm", ValueInput::createByReal(S.leadA_cm));
            leadA->minimumValue(0.0);

            // Feature A Type
            Ptr<DropDownCommandInput> ddA = giA->addDropDownCommandInput(kFeatATypeId, "Feature A Type", DropDownStyles::TextListDropDownStyle);
            Ptr<ListItems> itemsA = ddA->listItems();
            itemsA->add("None", S.featAType == "None");
            itemsA->add("Arrow", S.featAType == "Arrow");
            itemsA->add("T", S.featAType == "T");

            // Feature A Width / Length
            Ptr<ValueCommandInput> aW = giA->addValueInput(kFeatAWidthId, "Feature A Width", "mm", ValueInput::createByReal(S.featAW_cm));
            Ptr<ValueCommandInput> aL = giA->addValueInput(kFeatALengthId, "Feature A Length", "mm", ValueInput::createByReal(S.featAL_cm));
			aW->minimumValue(0.0);
			aL->minimumValue(0.0);
            aW->isEnabled(false);
            aL->isEnabled(false);
        }

        // ---- Point B block ----
        {
            // Select Point B
            Ptr<SelectionCommandInput> selB = giB->addSelectionInput(kSelPointBId, "Select Point B", "Pick the end point (B)");
            addPointSelectionFilters(selB);

            // Lead B
            Ptr<ValueCommandInput> leadB = giB->addValueInput(kLeadBId, "Lead B", "mm", ValueInput::createByReal(S.leadB_cm));
            leadB->minimumValue(0.0);

            // Feature B Type
            Ptr<DropDownCommandInput> ddB = giB->addDropDownCommandInput(kFeatBTypeId, "Feature B Type", DropDownStyles::TextListDropDownStyle);
            Ptr<ListItems> itemsB = ddB->listItems();
            itemsB->add("None", S.featBType == "None");
            itemsB->add("Arrow", S.featBType == "Arrow");
            itemsB->add("T", S.featBType == "T");

            // Feature B Width / Length
            Ptr<ValueCommandInput> bW = giB->addValueInput(kFeatBWidthId, "Feature B Width", "mm", ValueInput::createByReal(S.featBW_cm));
            Ptr<ValueCommandInput> bL = giB->addValueInput(kFeatBLengthId, "Feature B Length", "mm", ValueInput::createByReal(S.featBL_cm));
			bW->minimumValue(0.0);
			bL->minimumValue(0.0);
            bW->isEnabled(false);
            bL->isEnabled(false);
        }

		Ptr<TextBoxCommandInput> errorBox = inputs->addTextBoxCommandInput(kErrorBox, "", "", 2, true);
		errorBox->isFullWidth(true);
        errorBox->isVisible(false); // hidden by default

        // Wire event handlers to the command.
        Ptr<InputChangedEvent> inputChangedEvent = cmd->inputChanged();
        if (!inputChangedEvent)
            return;
        if (!inputChangedEvent->add(&_thickLineInputChangedHandler))
      	    return;

		Ptr<ValidateInputsEvent> validateInputsEvent = cmd->validateInputs();
        if (!validateInputsEvent)
			return;
        if (!validateInputsEvent->add(&_thickLineValidateInputsHandler))
			return;

		Ptr<CommandEvent> commandEvent = cmd->execute();
        if (!commandEvent)
			return;
		if (!commandEvent->add(&_thickLineCommandHandler))
			return;

        // Initial pass so defaults match the selected items when the dialog opens
        updateFeatureInputs(inputs, kFeatATypeId, kFeatAWidthId, kFeatALengthId);
        updateFeatureInputs(inputs, kFeatBTypeId, kFeatBWidthId, kFeatBLengthId);
    }
} _thickLineCommandCreatedHandler;

extern "C" XI_EXPORT bool run(const char* context)
{
    _app = Application::get();
    if (!_app)
        return false;

    _ui = _app->userInterface();
    if (!_ui)
        return false;

    LogFusion("Thick Line Add-In started.\n");

    // Create a command definition and add a button to the CREATE panel.
    Ptr<CommandDefinition> cmdDef = _ui->commandDefinitions()->addButtonDefinition(
        "habiThickLineAddIn", "Thick Line", "Creates a Thick Line with features", "Resources/Icons");
    if (!cmdDef)
        return false;

    Ptr<ToolbarPanel> createPanel = _ui->allToolbarPanels()->itemById("SketchCreatePanel");
    if (!createPanel)
        return false;

    Ptr<CommandControl> gearButton = createPanel->controls()->addCommand(cmdDef);
    if (!gearButton)
        return false;
    // Promote it to show as a top-level icon in the panel
    gearButton->isPromoted(true);              // promote this instance now

    // Connect to the command created event.
    Ptr<CommandCreatedEvent> commandCreatedEvent = cmdDef->commandCreated();
    if (!commandCreatedEvent)
        return false;

    bool isOk = commandCreatedEvent->add(&_thickLineCommandCreatedHandler);
    if (!isOk)
        return false;

    std::string strContext = context;
    if (strContext.find("IsApplicationStartup", 0) != std::string::npos)
    {
        if (strContext.find("false", 0) != std::string::npos)
            LogFusion("The \"Thick Line\" command has been added\nto the CREATE panel of the SKETCH workspace.");
    }

    return true;
}

extern "C" XI_EXPORT bool stop(const char* context)
{
    if (_ui)
    {
        Ptr<ToolbarPanel> createPanel = _ui->allToolbarPanels()->itemById("SketchCreatePanel");
        if (!createPanel)
            return false;

        Ptr<CommandControl> gearButton = createPanel->controls()->itemById("habiThickLineAddIn");
        if (gearButton)
            gearButton->deleteMe();

        Ptr<CommandDefinition> cmdDef = _ui->commandDefinitions()->itemById("habiThickLineAddIn");
        if (cmdDef)
            cmdDef->deleteMe();

		LogFusion("Thick Line Add-In stopped.\n");
    }

    return true;
}
