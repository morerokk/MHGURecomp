// The Settings window (plat_settings_show: Cmd-, or Settings... in the Settings menu): every setting of
// mhgurecomp.ini, built from the settings table (runtime/config.c g_setting_rows).
//
// A tab per table tab (Display, Graphics, Controls, Multiplayer, Monster HP, Extras), a row per setting: a pop-up for
// a fixed choice, an editable combo box with suggestions for a value, a text field for free text, and under it the
// explanation the default file gives (config_help). A change is written at once (config_set: the parser checks it
// first, a value it doesn't take is refused); a live one takes effect at once (runtime/ui.c ui_setting_changed), the
// others are marked until "Restart to Apply". The Monster HP tab also lists the per-quest Quest<number> entries in a
// table, to add, change and remove.
#import <Cocoa/Cocoa.h>

#include "../app_actions.h"
#include "../config.h"
#include "darwin_ui.h"

void ui_setting_changed(const char *key);  // (runtime/ui.c)

static NSTextField *label(NSString *text, CGFloat size, bool bold, bool secondary) {
    NSTextField *l = [NSTextField wrappingLabelWithString:text];
    l.font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
    if (secondary) l.textColor = NSColor.secondaryLabelColor;
    l.selectable = NO;
    return l;
}

// A rounded, lightly filled card around its content (sized by it).
@interface MHGUCard : NSView
@end
@implementation MHGUCard
- (void)drawRect:(NSRect)r {
    (void)r;
    [[NSColor.labelColor colorWithAlphaComponent:0.06] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:10 yRadius:10] fill];
}
@end

// A tab's content from the top (a scroll view's own coordinates start at the bottom).
@interface MHGUFlipped : NSView
@end
@implementation MHGUFlipped
- (BOOL)isFlipped { return YES; }
@end

// One setting's row: its control and the "after restart" note beside it.
@interface MHGURow : NSObject
@property(assign) const SettingRow *setting;
@property(strong) NSControl *control;
@property(strong) NSTextField *note;
@end
@implementation MHGURow
@end

@interface MHGUSettingsWindow : NSObject <NSWindowDelegate, NSComboBoxDelegate, NSTextFieldDelegate,
                                          NSTableViewDataSource, NSTableViewDelegate>
@property(strong) NSWindow *window;
@property(strong) NSMutableArray<MHGURow *> *rows;
@property(strong) NSButton *restartButton;
@property(strong) NSTableView *quests;
@property(strong) NSMutableArray<NSMutableArray *> *questRows;  // [quest number, factor 1..4 as text]
@property(strong) NSTextField *questsEmpty;           // (shown over the table while it has no rows)
@property(strong) NSSegmentedControl *questButtons;  // + / -
@property(strong) NSMutableArray<NSSlider *> *hunterSliders;
@property(strong) NSMutableArray<NSTextField *> *hunterFields;
@property(strong) NSString *lastHelp;  // (the row above's: rows that share it, as the keybinds, show it once)
@end

static MHGUSettingsWindow *g_settings_window;

@implementation MHGUSettingsWindow

// ---- values
- (NSString *)valueOf:(const SettingRow *)st {
    char now[256] = "";
    config_get(st->key, now, sizeof now);
    return str(now);
}

- (void)reload {
    for (MHGURow *row in self.rows) {
        const SettingRow *st = row.setting;
        NSString *now = [self valueOf:st];
        if ([row.control isKindOfClass:NSPopUpButton.class]) {
            NSPopUpButton *p = (NSPopUpButton *)row.control;
            NSInteger at = -1;
            for (NSInteger i = 0; i < p.numberOfItems; i++)
                if (config_same_value(st->key, now.UTF8String, [[p itemAtIndex:i].representedObject UTF8String])) at = i;
            if (at < 0) {  // (a value the file has that isn't one of the choices: shown as it is)
                [p addItemWithTitle:now];
                p.lastItem.representedObject = now;
                at = p.numberOfItems - 1;
            }
            [p selectItemAtIndex:at];
        } else if (row.control.currentEditor == nil) {  // (not while the player is typing in it)
            row.control.stringValue = now;
            ((NSTextField *)row.control).textColor = NSColor.controlTextColor;
        }
        bool pending = !st->live && config_restart_pending(st->key);
        row.note.stringValue = pending ? @"after restart" : @"";
    }
    self.restartButton.enabled = config_restart_pending(NULL);
    [self loadQuests];
}

- (void)apply:(MHGURow *)row value:(NSString *)value {
    const SettingRow *st = row.setting;
    if (config_same_value(st->key, [self valueOf:st].UTF8String, value.UTF8String)) return;
    if (!config_set(st->key, value.UTF8String)) {  // (not a value it takes: refused, the file's value shown again)
        NSBeep();
        row.note.stringValue = @"not a value this takes";
        row.note.textColor = NSColor.systemRedColor;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            row.note.textColor = NSColor.secondaryLabelColor;
            [self reload];
        });
        if ([row.control isKindOfClass:NSPopUpButton.class]) [self reload];
        return;
    }
    if (st->live) ui_setting_changed(st->key);
    [self reload];
}

- (MHGURow *)rowFor:(id)control {
    for (MHGURow *row in self.rows) if (row.control == control) return row;
    return nil;
}

- (void)popupChanged:(NSPopUpButton *)p { [self apply:[self rowFor:p] value:p.selectedItem.representedObject]; }
- (void)fieldChanged:(NSTextField *)f {
    MHGURow *row = [self rowFor:f];
    if (row) [self apply:row value:[f.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]];
}
- (void)controlTextDidEndEditing:(NSNotification *)n {
    if ([self rowFor:n.object]) [self fieldChanged:n.object];
}
- (void)comboBoxSelectionDidChange:(NSNotification *)n {
    NSComboBox *c = n.object;
    if (c.indexOfSelectedItem >= 0) {
        c.stringValue = [c itemObjectValueAtIndex:c.indexOfSelectedItem];
        [self fieldChanged:c];
    }
}

// ---- Monster HP: the factors for 1-4 hunters as tiles (a slider and a percentage each), the per-quest exceptions as a
// table with the quests' names (from the game's own text), the explanation behind an info button.
extern const char *g_game_dir;  // (runtime/main.c)

// A quest's title from the game's text (romfs nativeNX/eng/quest/questData/questData_<n>_eng.gmd: the first string of
// its string block, whose size is at +0x20). nil: no such quest.
static NSString *quest_name(unsigned q) {
    static NSMutableDictionary<NSNumber *, NSString *> *cache;
    if (!cache) cache = [NSMutableDictionary dictionary];
    if (cache[@(q)]) return cache[@(q)].length ? cache[@(q)] : nil;
    char path[1400];
    snprintf(path, sizeof path, "%s/romfs/nativeNX/eng/quest/questData/questData_%07u_eng.gmd", g_game_dir, q);
    NSData *d = [NSData dataWithContentsOfFile:str(path)];
    NSString *name = @"";
    if (d.length > 0x28) {
        const uint8_t *bytes = d.bytes;
        uint32_t block;
        memcpy(&block, bytes + 0x20, 4);
        if (block && block < d.length - 0x28) {
            const char *t = (const char *)bytes + d.length - block;
            name = [[NSString alloc] initWithBytes:t length:strnlen(t, block) encoding:NSUTF8StringEncoding] ?: @"";
        }
    }
    cache[@(q)] = name;
    return name.length ? name : nil;
}

static NSString *percent(double f) { return [NSString stringWithFormat:@"%.0f %%", f * 100.0]; }

// A factor as the file writes it: two decimals at most, at least one ("1.0", "1.25").
static NSString *factor_text(double f) {
    NSString *t = [NSString stringWithFormat:@"%.2f", round(f * 100.0) / 100.0];
    if ([t hasSuffix:@"0"]) t = [t substringToIndex:t.length - 1];
    return t;
}

// What the player typed: "120 %", "120" or "1.2" (a percentage when marked so or above 5). false: not a number > 0.
static bool parse_amount(NSString *s, double *f) {
    NSString *t = [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    bool pct = [t hasSuffix:@"%"];
    t = [[t stringByReplacingOccurrencesOfString:@"%" withString:@""] stringByReplacingOccurrencesOfString:@"," withString:@"."];
    NSScanner *sc = [NSScanner scannerWithString:t];
    double v;
    if (![sc scanDouble:&v] || !sc.atEnd || v <= 0) return false;
    *f = pct || v > 5 ? v / 100.0 : v;
    return true;
}

static const char *const hunter_keys[4] = {"1Player", "2Players", "3Players", "4Players"};

- (void)loadHunters {
    for (int i = 0; i < 4; i++) {
        NSSlider *sl = self.hunterSliders[i];
        NSTextField *f = self.hunterFields[i];
        char now[64] = "1.0";
        config_get(hunter_keys[i], now, sizeof now);
        double v = atof(now);
        sl.doubleValue = v;
        if (!f.currentEditor) f.stringValue = percent(v);
    }
}

- (void)saveHunter:(int)i factor:(double)v {
    if (!config_set(hunter_keys[i], factor_text(v).UTF8String)) NSBeep();
    [self loadHunters];
}

- (void)hunterSlid:(NSSlider *)sl {
    int i = (int)sl.tag;
    double v = round(sl.doubleValue * 20.0) / 20.0;  // (5 % steps)
    self.hunterFields[i].stringValue = percent(v);
    if (NSApp.currentEvent.type != NSEventTypeLeftMouseDragged) [self saveHunter:i factor:v];  // (saved on release)
}

- (void)hunterTyped:(NSTextField *)f {
    double v;
    if (parse_amount(f.stringValue, &v)) [self saveHunter:(int)f.tag factor:v];
    else { NSBeep(); [self loadHunters]; }
}

static void add_quest(unsigned quest, const char *value, void *ctx) {
    NSMutableArray *rows = (__bridge NSMutableArray *)ctx;
    NSArray<NSString *> *f = [[str(value) stringByReplacingOccurrencesOfString:@" " withString:@""]
                                 componentsSeparatedByString:@","];
    NSMutableArray *row = [NSMutableArray arrayWithObject:@(quest)];
    for (int i = 0; i < 4; i++) [row addObject:f.count == 4 ? f[i] : f[0]];
    [rows addObject:row];
}

- (void)loadQuests {
    [self loadHunters];
    if (!self.quests || self.quests.editedRow >= 0) return;
    NSMutableArray *rows = [NSMutableArray array];
    config_quests(add_quest, (__bridge void *)rows);
    for (NSMutableArray *r in self.questRows)  // (rows being added: no quest number yet, so not in the file)
        if ([r[0] unsignedIntValue] == 0) [rows addObject:r];
    self.questRows = rows;
    [self.quests reloadData];
    self.questsEmpty.hidden = rows.count > 0;
    [self.questButtons setEnabled:self.quests.selectedRow >= 0 forSegment:1];
}

- (bool)saveQuest:(NSMutableArray *)r {
    unsigned q = [r[0] unsignedIntValue];
    if (!q) return true;
    NSString *v = ([r[1] isEqual:r[2]] && [r[1] isEqual:r[3]] && [r[1] isEqual:r[4]])
                      ? r[1] : [NSString stringWithFormat:@"%@, %@, %@, %@", r[1], r[2], r[3], r[4]];
    char key[32];
    snprintf(key, sizeof key, "Quest%u", q);
    return config_set(key, v.UTF8String);
}

// Columns: "quest", "name", "1".."4" (the factors, shown as percentages)
- (NSInteger)numberOfRowsInTableView:(NSTableView *)t { (void)t; return (NSInteger)self.questRows.count; }
- (id)tableView:(NSTableView *)t objectValueForTableColumn:(NSTableColumn *)col row:(NSInteger)i {
    (void)t;
    NSArray *r = self.questRows[(NSUInteger)i];
    unsigned q = [r[0] unsignedIntValue];
    if ([col.identifier isEqual:@"quest"]) return q ? [r[0] stringValue] : @"";
    if ([col.identifier isEqual:@"name"]) return q ? (quest_name(q) ?: @"Unknown quest") : @"Type the quest's number";
    return percent([r[(NSUInteger)col.identifier.integerValue] doubleValue]);
}
- (void)tableView:(NSTableView *)t willDisplayCell:(id)cell forTableColumn:(NSTableColumn *)col row:(NSInteger)i {
    (void)t;
    if (![col.identifier isEqual:@"name"]) return;
    unsigned q = [self.questRows[(NSUInteger)i][0] unsignedIntValue];
    [cell setTextColor:q && quest_name(q) ? NSColor.secondaryLabelColor : NSColor.systemOrangeColor];
}
- (void)tableView:(NSTableView *)t setObjectValue:(id)v forTableColumn:(NSTableColumn *)col row:(NSInteger)i {
    (void)t;
    NSMutableArray *r = self.questRows[(NSUInteger)i];
    NSMutableArray *old = [r mutableCopy];
    NSString *s = [[v description] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if ([col.identifier isEqual:@"quest"]) {
        r[0] = @((unsigned)s.integerValue > 0 ? (unsigned)s.integerValue : 0);
    } else {
        double f;
        if (!parse_amount(s, &f)) { NSBeep(); return; }
        r[(NSUInteger)col.identifier.integerValue] = factor_text(f);
    }
    if (![self saveQuest:r]) {  // (not factors the setting takes: as it was)
        NSBeep();
        self.questRows[(NSUInteger)i] = old;
    } else if ([old[0] unsignedIntValue] && [old[0] unsignedIntValue] != [r[0] unsignedIntValue]) {
        char key[32];  // (a new quest number: the old entry goes)
        snprintf(key, sizeof key, "Quest%u", [old[0] unsignedIntValue]);
        config_remove(key);
    }
    dispatch_async(dispatch_get_main_queue(), ^{ [self loadQuests]; });
}
- (void)tableViewSelectionDidChange:(NSNotification *)n {
    (void)n;
    [self.questButtons setEnabled:self.quests.selectedRow >= 0 forSegment:1];
}

- (void)questButton:(NSSegmentedControl *)sc {
    if (sc.selectedSegment == 0) {  // +: a row to type a quest number into
        [self.questRows addObject:[NSMutableArray arrayWithObjects:@0, @"1.0", @"1.0", @"1.0", @"1.0", nil]];
        [self.quests reloadData];
        self.questsEmpty.hidden = YES;
        NSInteger i = (NSInteger)self.questRows.count - 1;
        [self.quests selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)i] byExtendingSelection:NO];
        [self.quests editColumn:0 row:i withEvent:nil select:YES];
        return;
    }
    NSInteger i = self.quests.selectedRow;  // -: the selected quest
    if (i < 0) return;
    unsigned q = [self.questRows[(NSUInteger)i][0] unsignedIntValue];
    if (q) {
        char key[32];
        snprintf(key, sizeof key, "Quest%u", q);
        config_remove(key);
    }
    [self.questRows removeObjectAtIndex:(NSUInteger)i];
    [self loadQuests];
}

// A rounded, lightly filled card with a title (and a line under it).
static NSView *card(NSString *title, NSString *subtitle, NSView *content) {
    NSStackView *inner = [NSStackView stackViewWithViews:@[ label(title, 13, true, false) ]];
    inner.orientation = NSUserInterfaceLayoutOrientationVertical;
    inner.alignment = NSLayoutAttributeLeading;
    inner.spacing = 4;
    if (subtitle) [inner addArrangedSubview:label(subtitle, 11, false, true)];
    [inner setCustomSpacing:12 afterView:inner.arrangedSubviews.lastObject];
    [inner addArrangedSubview:content];
    [content.widthAnchor constraintEqualToAnchor:inner.widthAnchor].active = YES;  // (the content as wide as the card)
    MHGUCard *box = [MHGUCard new];
    inner.translatesAutoresizingMaskIntoConstraints = NO;
    [box addSubview:inner];
    [inner.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:16].active = YES;
    [inner.trailingAnchor constraintEqualToAnchor:box.trailingAnchor constant:-16].active = YES;
    [inner.topAnchor constraintEqualToAnchor:box.topAnchor constant:14].active = YES;
    [inner.bottomAnchor constraintEqualToAnchor:box.bottomAnchor constant:-14].active = YES;
    return box;
}

// i people, as SF Symbols side by side
static NSView *people(int n) {
    NSMutableArray *icons = [NSMutableArray array];
    for (int i = 0; i < n; i++) {
        NSImageView *iv = [NSImageView imageViewWithImage:[NSImage imageWithSystemSymbolName:@"person.fill" accessibilityDescription:nil]];
        iv.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:15 weight:NSFontWeightSemibold];
        iv.contentTintColor = NSColor.controlAccentColor;
        [icons addObject:iv];
    }
    NSStackView *row = [NSStackView stackViewWithViews:icons];
    row.spacing = 1;
    [row.heightAnchor constraintEqualToConstant:22].active = YES;
    return row;
}

- (NSView *)hunterTiles {
    NSMutableArray *tiles = [NSMutableArray array];
    self.hunterSliders = [NSMutableArray array];
    self.hunterFields = [NSMutableArray array];
    for (int i = 0; i < 4; i++) {
        NSSlider *sl = [NSSlider sliderWithValue:1.0 minValue:0.25 maxValue:3.0 target:self action:@selector(hunterSlid:)];
        sl.tag = i;
        sl.continuous = YES;
        sl.numberOfTickMarks = 12;  // (every 25 %)
        sl.allowsTickMarkValuesOnly = NO;
        [sl.widthAnchor constraintEqualToConstant:118].active = YES;
        NSTextField *f = [NSTextField textFieldWithString:@"100 %"];
        f.tag = i;
        f.alignment = NSTextAlignmentCenter;
        f.font = [NSFont monospacedDigitSystemFontOfSize:15 weight:NSFontWeightMedium];
        f.bezelStyle = NSTextFieldRoundedBezel;
        f.target = self;
        f.action = @selector(hunterTyped:);
        [f.widthAnchor constraintEqualToConstant:76].active = YES;
        [self.hunterSliders addObject:sl];
        [self.hunterFields addObject:f];
        NSTextField *name = label(i ? [NSString stringWithFormat:@"%d Hunters", i + 1] : @"1 Hunter", 12, false, true);
        NSStackView *tile = [NSStackView stackViewWithViews:@[ people(i + 1), name, f, sl ]];
        tile.orientation = NSUserInterfaceLayoutOrientationVertical;
        tile.alignment = NSLayoutAttributeCenterX;
        tile.spacing = 7;
        [tiles addObject:tile];
    }
    NSStackView *row = [NSStackView stackViewWithViews:tiles];
    row.distribution = NSStackViewDistributionFillEqually;
    row.spacing = 12;
    return row;
}

- (NSView *)questTable {
    NSTableView *t = [[NSTableView alloc] init];
    struct { NSString *ident, *title; CGFloat width; bool edit; } cols[] = {
        {@"quest", @"Quest", 64, true}, {@"name", @"Name", 200, false}, {@"1", @"1 Hunter", 70, true},
        {@"2", @"2 Hunters", 70, true}, {@"3", @"3 Hunters", 70, true}, {@"4", @"4 Hunters", 70, true}};
    for (size_t c = 0; c < sizeof cols / sizeof cols[0]; c++) {
        NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:cols[c].ident];
        col.title = cols[c].title;
        col.width = cols[c].width;
        col.editable = cols[c].edit;
        if (c >= 2) ((NSCell *)col.dataCell).alignment = NSTextAlignmentRight, col.headerCell.alignment = NSTextAlignmentRight;
        [t addTableColumn:col];
    }
    [t moveColumn:1 toColumn:0];  // (Name first for the resizing, then back: it is the one that grows)
    [t moveColumn:0 toColumn:1];
    t.dataSource = self;
    t.delegate = self;
    t.usesAlternatingRowBackgroundColors = YES;
    t.rowHeight = 22;
    t.columnAutoresizingStyle = NSTableViewFirstColumnOnlyAutoresizingStyle;  // (the name takes the room left)
    self.quests = t;
    NSScrollView *sv = [[NSScrollView alloc] init];
    sv.documentView = t;
    sv.hasVerticalScroller = YES;
    sv.borderType = NSBezelBorder;
    // (the table in a container, with the empty table's line centred over its rows: a scroll view places its own
    // subviews, so the line can't go into it)
    NSView *wrap = [NSView new];
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    [wrap addSubview:sv];
    [sv.leadingAnchor constraintEqualToAnchor:wrap.leadingAnchor].active = YES;
    [sv.trailingAnchor constraintEqualToAnchor:wrap.trailingAnchor].active = YES;
    [sv.topAnchor constraintEqualToAnchor:wrap.topAnchor].active = YES;
    [sv.bottomAnchor constraintEqualToAnchor:wrap.bottomAnchor].active = YES;
    [sv.heightAnchor constraintEqualToConstant:140].active = YES;
    self.questsEmpty = label(@"No quests set apart. Click + and type a quest's number.", 12, false, true);
    self.questsEmpty.translatesAutoresizingMaskIntoConstraints = NO;
    [wrap addSubview:self.questsEmpty positioned:NSWindowAbove relativeTo:sv];
    [self.questsEmpty.centerXAnchor constraintEqualToAnchor:wrap.centerXAnchor].active = YES;
    [self.questsEmpty.centerYAnchor constraintEqualToAnchor:wrap.centerYAnchor constant:12].active = YES;  // (below the header)
    self.questButtons = [NSSegmentedControl segmentedControlWithImages:@[
        [NSImage imageNamed:NSImageNameAddTemplate], [NSImage imageNamed:NSImageNameRemoveTemplate]
    ] trackingMode:NSSegmentSwitchTrackingMomentary target:self action:@selector(questButton:)];
    self.questButtons.segmentStyle = NSSegmentStyleSmallSquare;
    NSTextField *tip = label(@"These replace the factors above for that quest. log.txt names each quest's number.",
                             11, false, false);
    NSStackView *bar = [NSStackView stackViewWithViews:@[ self.questButtons, tip ]];
    bar.spacing = 12;
    NSStackView *v = [NSStackView stackViewWithViews:@[ wrap, bar ]];
    v.orientation = NSUserInterfaceLayoutOrientationVertical;
    v.alignment = NSLayoutAttributeLeading;
    v.spacing = 0;
    [v setCustomSpacing:6 afterView:wrap];
    [wrap.widthAnchor constraintEqualToAnchor:v.widthAnchor].active = YES;
    return v;
}

- (NSView *)monsterHPView {
    NSTextField *head = label(@"Large monsters' HP in Hub quests, by how many hunters take the quest. 100 % is the original "
                              @"game.", 12, false, true);
    NSView *hunters = card(@"By Number of Hunters", nil, [self hunterTiles]);
    NSView *quests = card(@"Quest Exceptions", @"Quests with factors of their own.", [self questTable]);
    NSTextField *foot = label(@"Only HP changes. Applies from the next quest; in a hub, everyone uses the host's values.", 11, false, true);
    NSStackView *v = [NSStackView stackViewWithViews:@[ head, hunters, quests, foot ]];
    v.orientation = NSUserInterfaceLayoutOrientationVertical;
    v.alignment = NSLayoutAttributeLeading;
    v.spacing = 14;
    v.edgeInsets = NSEdgeInsetsMake(18, 20, 18, 20);
    for (NSView *c in @[ head, hunters, quests ]) [c.widthAnchor constraintEqualToAnchor:v.widthAnchor constant:-40].active = YES;
    return v;
}

// ---- building
- (NSControl *)controlFor:(const SettingRow *)st {
    NSArray<NSString *> *values = split(st->choices), *labels = split(st->labels);
    if (labels.count != values.count) labels = values;
    if (st->kind == SETTING_CHOICE) {
        NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        for (NSUInteger i = 0; i < values.count; i++) {
            [p addItemWithTitle:labels[i]];
            p.lastItem.representedObject = values[i];
        }
        p.target = self;
        p.action = @selector(popupChanged:);
        return p;
    }
    NSTextField *f;
    if (st->kind == SETTING_VALUE) {
        NSComboBox *c = [[NSComboBox alloc] init];
        NSMutableArray *items = [values mutableCopy];
        const char *here = plat_screen_size();
        if (!strcmp(st->key, "Resolution") && here && ![items containsObject:str(here)]) [items insertObject:str(here) atIndex:1];
        [c addItemsWithObjectValues:items];
        c.numberOfVisibleItems = 10;
        c.delegate = self;
        f = c;
    } else {
        f = [[NSTextField alloc] init];
        f.placeholderString = !strcmp(st->key, "Join") ? @"203.0.113.7:39500, friend.example.net" : @"";
        f.delegate = self;
    }
    f.target = self;
    f.action = @selector(fieldChanged:);
    return f;
}

- (NSView *)rowView:(const SettingRow *)st {
    MHGURow *row = [MHGURow new];
    row.setting = st;
    row.control = [self controlFor:st];
    row.note = label(@"", 11, false, true);
    [self.rows addObject:row];
    NSTextField *title = label(str(st->title), 13, false, false);
    title.alignment = NSTextAlignmentRight;
    [title.widthAnchor constraintEqualToConstant:200].active = YES;
    [row.control.widthAnchor constraintEqualToConstant:st->kind == SETTING_TEXT ? 320 : 230].active = YES;
    NSStackView *line = [NSStackView stackViewWithViews:@[ title, row.control, row.note ]];
    line.spacing = 10;
    NSStackView *box = [NSStackView stackViewWithViews:@[ line ]];
    box.orientation = NSUserInterfaceLayoutOrientationVertical;
    box.alignment = NSLayoutAttributeLeading;
    box.spacing = 3;
    char help[2048];
    if (config_help(st->key, help, sizeof help) && ![str(help) isEqual:self.lastHelp]) {
        self.lastHelp = str(help);
        NSTextField *h = label(str(help), 11, false, true);
        h.preferredMaxLayoutWidth = 470;
        NSStackView *indent = [NSStackView stackViewWithViews:@[ h ]];
        indent.edgeInsets = NSEdgeInsetsMake(0, 210, 0, 0);
        [box addArrangedSubview:indent];
    }
    return box;
}

- (NSView *)tabView:(NSString *)tab {
    if ([tab isEqual:@"Monster HP"]) return [self scrolled:[self monsterHPView]];
    NSStackView *stack = [NSStackView stackViewWithViews:@[]];
    self.lastHelp = nil;
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 14;
    stack.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);
    int sections = 0;
    const char *section = "";
    for (int i = 0; i < g_nsetting_rows; i++)
        if ([str(g_setting_rows[i].tab) isEqual:tab] && strcmp(g_setting_rows[i].section, section)) sections++, section = g_setting_rows[i].section;
    section = "";
    for (int i = 0; i < g_nsetting_rows; i++) {
        const SettingRow *st = &g_setting_rows[i];
        if (![str(st->tab) isEqual:tab]) continue;
        if (sections > 1 && strcmp(st->section, section)) [stack addArrangedSubview:label(str(st->section), 13, true, false)];
        section = st->section;
        [stack addArrangedSubview:[self rowView:st]];
    }
    return [self scrolled:stack];
}

// (scrollable: a tab's explanations can be taller than the window)
- (NSView *)scrolled:(NSView *)stack {
    NSScrollView *sv = [[NSScrollView alloc] init];
    sv.hasVerticalScroller = YES;
    sv.drawsBackground = NO;
    NSClipView *clip = sv.contentView;
    MHGUFlipped *doc = [MHGUFlipped new];
    doc.translatesAutoresizingMaskIntoConstraints = NO;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [doc addSubview:stack];
    sv.documentView = doc;
    [doc.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [doc.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [doc.trailingAnchor constraintEqualToAnchor:clip.trailingAnchor].active = YES;
    [stack.leadingAnchor constraintEqualToAnchor:doc.leadingAnchor].active = YES;
    [stack.topAnchor constraintEqualToAnchor:doc.topAnchor].active = YES;
    [stack.trailingAnchor constraintEqualToAnchor:doc.trailingAnchor].active = YES;
    [stack.bottomAnchor constraintEqualToAnchor:doc.bottomAnchor].active = YES;
    return sv;
}

- (instancetype)init {
    if (!(self = [super init])) return nil;
    self.rows = [NSMutableArray array];
    self.questRows = [NSMutableArray array];
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 760, 660)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered defer:YES];
    w.title = @"MHGU Recomp Settings";
    w.releasedWhenClosed = NO;
    w.delegate = self;
    w.minSize = NSMakeSize(700, 420);
    // (over the game in full screen too)
    w.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorMoveToActiveSpace;
    w.level = NSFloatingWindowLevel;
    self.window = w;

    NSTabView *tabs = [[NSTabView alloc] init];
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (int i = 0; i < g_nsetting_rows; i++)
        if (![names containsObject:str(g_setting_rows[i].tab)]) [names addObject:str(g_setting_rows[i].tab)];
    for (NSString *name in names) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:name];
        item.label = name;
        item.view = [self tabView:name];
        [tabs addTabViewItem:item];
    }
    self.restartButton = [NSButton buttonWithTitle:@"Restart to Apply" target:self action:@selector(restart:)];
    NSButton *edit = [NSButton buttonWithTitle:@"Edit Settings File…" target:self action:@selector(edit:)];
    NSButton *folder = [NSButton buttonWithTitle:@"Open Settings Folder" target:self action:@selector(folder:)];
    NSTextField *hint = label(@"Changes are saved at once.", 11, false, true);
    NSStackView *footer = [NSStackView stackViewWithViews:@[ hint, edit, folder, self.restartButton ]];
    [footer setHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    footer.spacing = 10;
    NSStackView *all = [NSStackView stackViewWithViews:@[ tabs, footer ]];
    all.orientation = NSUserInterfaceLayoutOrientationVertical;
    all.alignment = NSLayoutAttributeCenterX;
    all.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
    [tabs.widthAnchor constraintEqualToAnchor:all.widthAnchor constant:-24].active = YES;
    [footer.widthAnchor constraintEqualToAnchor:tabs.widthAnchor].active = YES;  // (the note takes the room left)
    [hint setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    w.contentView = all;
    [w center];
    return self;
}

- (void)windowDidBecomeKey:(NSNotification *)n { (void)n; [self reload]; }
- (void)restart:(id)sender { (void)sender; ui_request_action(true); }
- (void)edit:(id)sender { (void)sender; plat_edit_settings(); }
- (void)folder:(id)sender { (void)sender; plat_open_home(); }
@end

void plat_settings_show(void) {
    if (!g_settings_window) g_settings_window = [MHGUSettingsWindow new];
    [g_settings_window reload];
    [g_settings_window.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

// (debug, MHGU_CONTROL "settingsshot <folder>") each tab of the window as <folder>/settings_<n>.png
void plat_settings_snapshot(const char *dir) {
    plat_settings_show();
    NSWindow *w = g_settings_window.window;
    NSTabView *tabs = nil;
    for (NSView *v in w.contentView.subviews) if ([v isKindOfClass:NSTabView.class]) tabs = (NSTabView *)v;
    for (NSInteger i = 0; tabs && i < tabs.numberOfTabViewItems; i++) {
        [tabs selectTabViewItemAtIndex:i];
        [w.contentView layoutSubtreeIfNeeded];
        [w displayIfNeeded];
        NSView *v = w.contentView;
        NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
        NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = ctx;
        [v.effectiveAppearance performAsCurrentDrawingAppearance:^{  // (the window's background, as it shows)
            [NSColor.windowBackgroundColor setFill];
            NSRectFill(v.bounds);
        }];
        [NSGraphicsContext restoreGraphicsState];
        [v displayRectIgnoringOpacity:v.bounds inContext:ctx];
        NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        [png writeToFile:[NSString stringWithFormat:@"%s/settings_%ld.png", dir, (long)i] atomically:YES];
    }
}

void plat_settings_refresh(void) {
    if (g_settings_window && g_settings_window.window.visible) [g_settings_window reload];
}
