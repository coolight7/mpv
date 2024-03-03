/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

extension MenuBar {
    enum MenuKey {
        case normalSize
        case halfSize
        case doubleSize
        case minimize
        case zoom
    }

    struct Config {
        let name: String
        let key: String
        let modifiers: NSEvent.ModifierFlags
        let action: Selector?
        let target: AnyObject?
        let command: String
        let url: String?
        let file: String?
        let alertTitle1: String?
        let alertText1: String?
        let alertTitle2: String?
        let alertText2: String?
        let alertTitle3: String?
        let alertText3: String?
        let commandSpecial: MenuKey?
        var menuItem: NSMenuItem?
        var configs: [Config]?

        init(
            name: String = "",
            key: String = "",
            modifiers: NSEvent.ModifierFlags = .command,
            action: Selector? = nil,
            target: AnyObject? = nil,
            command: String = "",
            url: String? = nil,
            file: String? = nil,
            alertTitle1: String? = nil,
            alertText1: String? = nil,
            alertTitle2: String? = nil,
            alertText2: String? = nil,
            alertTitle3: String? = nil,
            alertText3: String? = nil,
            commandSpecial: MenuKey? = nil,
            menuItem: NSMenuItem? = nil,
            configs: [Config]? = nil
        ) {
            self.name = name
            self.key = key
            self.modifiers = modifiers
            self.action = action
            self.target = target
            self.command = command
            self.url = url
            self.file = file
            self.alertTitle1 = alertTitle1
            self.alertText1 = alertText1
            self.alertTitle2 = alertTitle2
            self.alertText2 = alertText2
            self.alertTitle3 = alertTitle3
            self.alertText3 = alertText3
            self.commandSpecial = commandSpecial
            self.menuItem = menuItem
            self.configs = configs
        }
    }
}

class MenuBar: NSObject {
    var menuConfigs: [Config] = []
    let appIcon: NSImage

    @objc override init() {
        UserDefaults.standard.set(false, forKey: "NSFullScreenMenuItemEverywhere")
        UserDefaults.standard.set(true, forKey: "NSDisabledDictationMenuItem")
        UserDefaults.standard.set(true, forKey: "NSDisabledCharacterPaletteMenuItem")
        NSWindow.allowsAutomaticWindowTabbing = false
        appIcon = (NSApp as? Application)?.getMPVIcon() ?? NSImage(size: NSSize(width: 1, height: 1))

        super.init()

        let appMenuConfigs = [
            Config(name: "About mpv", action: #selector(about), target: self),
            Config(name: "separator"),
            Config(
                name: "Preferences…",
                key: ",",
                action: #selector(preferences(_:)),
                target: self,
                file: "mpv.conf",
                alertTitle1: "No Application found to open your config file.",
                alertText1: "Please open the mpv.conf file with your preferred text editor in the now open folder to edit your config.",
                alertTitle2: "No config file found.",
                alertText2: "Please create a mpv.conf file with your preferred text editor in the now open folder.",
                alertTitle3: "No config path or file found.",
                alertText3: "Please create the following path ~/.config/mpv/ and a mpv.conf file within with your preferred text editor."
            ),
            Config(
                name: "Keyboard Shortcuts Config…",
                action: #selector(preferences(_:)),
                target: self,
                file: "input.conf",
                alertTitle1: "No Application found to open your config file.",
                alertText1: "Please open the input.conf file with your preferred text editor in the now open folder to edit your config.",
                alertTitle2: "No config file found.",
                alertText2: "Please create a input.conf file with your preferred text editor in the now open folder.",
                alertTitle3: "No config path or file found.",
                alertText3: "Please create the following path ~/.config/mpv/ and a input.conf file within with your preferred text editor."
            ),
            Config(name: "separator"),
            Config(name: "Services"),
            Config(name: "separator"),
            Config(name: "Hide mpv", key: "h", action: #selector(NSApp.hide(_:))),
            Config(name: "Hide Others", key: "h", modifiers: [.command, .option], action: #selector(NSApp.hideOtherApplications(_:))),
            Config(name: "Show All", action: #selector(NSApp.unhideAllApplications(_:))),
            Config(name: "separator"),
            Config(name: "Quit and Remember Position", action: #selector(quit(_:)), target: self, command: "quit-watch-later"),
            Config(name: "Quit mpv", key: "q", action: #selector(quit(_:)), target: self, command: "quit"),
        ]

        let fileMenuConfigs = [
            Config(name: "Open File…", key: "o", action: #selector(openFile), target: self),
            Config(name: "Open URL…", key: "O", action: #selector(openUrl), target: self),
            Config(name: "Open Playlist…", action: #selector(openPlaylist), target: self),
            Config(name: "separator"),
            Config(name: "Close", key: "w", action: #selector(NSWindow.performClose(_:))),
            Config(name: "Save Screenshot", action: #selector(command(_:)), target: self, command: "async screenshot"),
        ]

        let editMenuConfigs = [
            Config(name: "Undo", key: "z", action: Selector(("undo:"))),
            Config(name: "Redo", key: "Z", action: Selector(("redo:"))),
            Config(name: "separator"),
            Config(name: "Cut", key: "x", action: #selector(NSText.cut(_:))),
            Config(name: "Copy", key: "c", action: #selector(NSText.copy(_:))),
            Config(name: "Paste", key: "v", action: #selector(NSText.paste(_:))),
            Config(name: "Select All", key: "a", action: #selector(NSResponder.selectAll(_:))),
        ]

        var viewMenuConfigs = [
            Config(name: "Toggle Fullscreen", action: #selector(command(_:)), target: self, command: "cycle fullscreen"),
            Config(name: "Toggle Float on Top", action: #selector(command(_:)), target: self, command: "cycle ontop"),
            Config(
                name: "Toggle Visibility on All Workspaces",
                action: #selector(command(_:)),
                target: self,
                command: "cycle on-all-workspaces"
            ),
        ]
#if HAVE_MACOS_TOUCHBAR
        viewMenuConfigs += [
            Config(name: "separator"),
            Config(name: "Customize Touch Bar…", action: #selector(NSApp.toggleTouchBarCustomizationPalette(_:))),
        ]
#endif

        let videoMenuConfigs = [
            Config(name: "Zoom Out", action: #selector(command(_:)), target: self, command: "add panscan -0.1"),
            Config(name: "Zoom In", action: #selector(command(_:)), target: self, command: "add panscan 0.1"),
            Config(name: "Reset Zoom", action: #selector(command(_:)), target: self, command: "set panscan 0"),
            Config(name: "separator"),
            Config(name: "Aspect Ratio 4:3", action: #selector(command(_:)), target: self, command: "set video-aspect-override \"4:3\""),
            Config(name: "Aspect Ratio 16:9", action: #selector(command(_:)), target: self, command: "set video-aspect-override \"16:9\""),
            Config(name: "Aspect Ratio 1.85:1", action: #selector(command(_:)), target: self, command: "set video-aspect-override \"1.85:1\""),
            Config(name: "Aspect Ratio 2.35:1", action: #selector(command(_:)), target: self, command: "set video-aspect-override \"2.35:1\""),
            Config(name: "Reset Aspect Ratio", action: #selector(command(_:)), target: self, command: "set video-aspect-override \"-1\""),
            Config(name: "separator"),
            Config(name: "Rotate Left", action: #selector(command(_:)), target: self, command: "cycle-values video-rotate 0 270 180 90"),
            Config(name: "Rotate Right", action: #selector(command(_:)), target: self, command: "cycle-values video-rotate 90 180 270 0"),
            Config(name: "Reset Rotation", action: #selector(command(_:)), target: self, command: "set video-rotate 0"),
            Config(name: "separator"),
            Config(name: "Half Size", key: "0", commandSpecial: .halfSize),
            Config(name: "Normal Size", key: "1", commandSpecial: .normalSize),
            Config(name: "Double Size", key: "2", commandSpecial: .doubleSize),
        ]

        let audioMenuConfigs = [
            Config(name: "Next Audio Track", action: #selector(command(_:)), target: self, command: "cycle audio"),
            Config(name: "Previous Audio Track", action: #selector(command(_:)), target: self, command: "cycle audio down"),
            Config(name: "separator"),
            Config(name: "Toggle Mute", action: #selector(command(_:)), target: self, command: "cycle mute"),
            Config(name: "separator"),
            Config(name: "Play Audio Later", action: #selector(command(_:)), target: self, command: "add audio-delay 0.1"),
            Config(name: "Play Audio Earlier", action: #selector(command(_:)), target: self, command: "add audio-delay -0.1"),
            Config(name: "Reset Audio Delay", action: #selector(command(_:)), target: self, command: "set audio-delay 0.0"),
        ]

        let subtitleMenuConfigs = [
            Config(name: "Next Subtitle Track", action: #selector(command(_:)), target: self, command: "cycle sub"),
            Config(name: "Previous Subtitle Track", action: #selector(command(_:)), target: self, command: "cycle sub down"),
            Config(name: "separator"),
            Config(name: "Toggle Force Style", action: #selector(command(_:)), target: self, command: "cycle-values sub-ass-override \"force\" \"no\""),
            Config(name: "separator"),
            Config(name: "Display Subtitles Later", action: #selector(command(_:)), target: self, command: "add sub-delay 0.1"),
            Config(name: "Display Subtitles Earlier", action: #selector(command(_:)), target: self, command: "add sub-delay -0.1"),
            Config(name: "Reset Subtitle Delay", action: #selector(command(_:)), target: self, command: "set sub-delay 0.0"),
        ]

        let playbackMenuConfigs = [
            Config(name: "Toggle Pause", action: #selector(command(_:)), target: self, command: "cycle pause"),
            Config(name: "Increase Speed", action: #selector(command(_:)), target: self, command: "add speed 0.1"),
            Config(name: "Decrease Speed", action: #selector(command(_:)), target: self, command: "add speed -0.1"),
            Config(name: "Reset Speed", action: #selector(command(_:)), target: self, command: "set speed 1.0"),
            Config(name: "separator"),
            Config(name: "Show Playlist", action: #selector(command(_:)), target: self, command: "script-message osc-playlist"),
            Config(name: "Show Chapters", action: #selector(command(_:)), target: self, command: "script-message osc-chapterlist"),
            Config(name: "Show Tracks", action: #selector(command(_:)), target: self, command: "script-message osc-tracklist"),
            Config(name: "separator"),
            Config(name: "Next File", action: #selector(command(_:)), target: self, command: "playlist-next"),
            Config(name: "Previous File", action: #selector(command(_:)), target: self, command: "playlist-prev"),
            Config(name: "Toggle Loop File", action: #selector(command(_:)), target: self, command: "cycle-values loop-file \"inf\" \"no\""),
            Config(name: "Toggle Loop Playlist", action: #selector(command(_:)), target: self, command: "cycle-values loop-playlist \"inf\" \"no\""),
            Config(name: "Shuffle", action: #selector(command(_:)), target: self, command: "playlist-shuffle"),
            Config(name: "separator"),
            Config(name: "Next Chapter", action: #selector(command(_:)), target: self, command: "add chapter 1"),
            Config(name: "Previous Chapter", action: #selector(command(_:)), target: self, command: "add chapter -1"),
            Config(name: "separator"),
            Config(name: "Step Forward", action: #selector(command(_:)), target: self, command: "frame-step"),
            Config(name: "Step Backward", action: #selector(command(_:)), target: self, command: "frame-back-step"),
        ]

        let windowMenuConfigs = [
            Config(name: "Minimize", key: "m", commandSpecial: .minimize),
            Config(name: "Zoom", key: "z", commandSpecial: .zoom),
        ]

        let helpMenuConfigs = [
            Config(name: "mpv Website…", action: #selector(url(_:)), target: self, url: "https://mpv.io"),
            Config(name: "mpv on GitHub…", action: #selector(url(_:)), target: self, url: "https://github.com/mpv-player/mpv"),
            Config(name: "separator"),
            Config(name: "Online Manual…", action: #selector(url(_:)), target: self, url: "https://mpv.io/manual/master/"),
            Config(name: "Online Wiki…", action: #selector(url(_:)), target: self, url: "https://github.com/mpv-player/mpv/wiki"),
            Config(name: "Release Notes…", action: #selector(url(_:)), target: self, url: "https://github.com/mpv-player/mpv/blob/master/RELEASE_NOTES"),
            Config(name: "Keyboard Shortcuts…", action: #selector(url(_:)), target: self, url: "https://github.com/mpv-player/mpv/blob/master/etc/input.conf"),
            Config(name: "separator"),
            Config(name: "Report Issue…", action: #selector(url(_:)), target: self, url: "https://github.com/mpv-player/mpv/issues/new/choose"),
            Config(
                name: "Show log File…",
                action: #selector(showFile(_:)),
                target: self,
                file: NSHomeDirectory() + "/Library/Logs/mpv.log",
                alertTitle1: "No log File found.",
                alertText1: "You deactivated logging for the Bundle."
            ),
        ]

        menuConfigs = [
            Config(name: "Apple", configs: appMenuConfigs),
            Config(name: "File", configs: fileMenuConfigs),
            Config(name: "Edit", configs: editMenuConfigs),
            Config(name: "View", configs: viewMenuConfigs),
            Config(name: "Video", configs: videoMenuConfigs),
            Config(name: "Audio", configs: audioMenuConfigs),
            Config(name: "Subtitle", configs: subtitleMenuConfigs),
            Config(name: "Playback", configs: playbackMenuConfigs),
            Config(name: "Window", configs: windowMenuConfigs),
            Config(name: "Help", configs: helpMenuConfigs),
        ]

        NSApp.mainMenu = generateMainMenu()
    }

    func generateMainMenu() -> NSMenu {
        let mainMenu = NSMenu(title: "MainMenu")
        NSApp.servicesMenu = NSMenu()

        for (menuConfigIndex, menuConfig) in menuConfigs.enumerated() {
            let menu = NSMenu(title: menuConfig.name)
            let item = NSMenuItem(title: menuConfig.name, action: nil, keyEquivalent: menuConfig.key)
            mainMenu.addItem(item)
            mainMenu.setSubmenu(menu, for: item)
            menuConfigs[menuConfigIndex].menuItem = item

            for (subConfigIndex, subConfig) in (menuConfig.configs ?? []).enumerated() {
#if HAVE_MACOS_TOUCHBAR
                if subConfig.action == "toggleTouchBarCustomizationPalette:" {
                    continue
                }
#endif

                if subConfig.name == "Show log File…" && ProcessInfo.processInfo.environment["MPVBUNDLE"] != "true" {
                    continue
                }

                if subConfig.name == "separator" {
                    menu.addItem(NSMenuItem.separator())
                } else {
                    let subItem = NSMenuItem(title: subConfig.name, action: subConfig.action, keyEquivalent: subConfig.key)
                    subItem.target = subConfig.target
                    subItem.keyEquivalentModifierMask = subConfig.modifiers
                    menu.addItem(subItem)
                    menuConfigs[menuConfigIndex].configs?[subConfigIndex].menuItem = subItem

                    if subConfig.name == "Services" {
                        subItem.submenu = NSApp.servicesMenu
                    }
                }
            }
        }

        return mainMenu
    }

    @objc func about() {
        NSApp.orderFrontStandardAboutPanel(options: [
            .applicationName: "mpv",
            .applicationIcon: appIcon,
            .applicationVersion: String(cString: swift_mpv_version),
            .init(rawValue: "Copyright"): String(cString: swift_mpv_copyright),
        ])
    }

    @objc func preferences(_ menuItem: NSMenuItem) {
        guard let menuConfig = getConfigFromMenu(menuItem: menuItem) else { return }
        let configPaths: [String] = [
            NSHomeDirectory() + "/.mpv/",
            NSHomeDirectory() + "/.config/mpv/",
        ]

        for path in configPaths {
            let configFile = path + (menuConfig.file ?? "")

            if FileManager.default.fileExists(atPath: configFile) {
                if NSWorkspace.shared.openFile(configFile) {
                    return
                }
                NSWorkspace.shared.openFile(path)
                alert(title: menuConfig.alertTitle1 ?? "", text: menuConfig.alertText1 ?? "")
                return
            }

            if NSWorkspace.shared.openFile(path) {
                alert(title: menuConfig.alertTitle2 ?? "", text: menuConfig.alertText2 ?? "")
                return
            }
        }

        alert(title: menuConfig.alertTitle3 ?? "", text: menuConfig.alertText3 ?? "")
    }

    @objc func quit(_ menuItem: NSMenuItem) {
        guard let menuConfig = getConfigFromMenu(menuItem: menuItem) else { return }
        menuConfig.command.withCString {
            (NSApp as? Application)?.stopMPV(UnsafeMutablePointer<CChar>(mutating: $0))
        }
    }

    @objc func openFile() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = true

        if panel.runModal() == .OK {
            var files: [String] = []
            for url in panel.urls {
                files += [url.path]
            }
            (NSApp as? Application)?.openFiles(files)
        }
    }

    @objc func openPlaylist() {
        let panel = NSOpenPanel()

        if panel.runModal() == .OK {
            "loadlist \"\(panel.urls[0].path)\"".withCString {
                (NSApp as? Application)?.queueCommand(UnsafeMutablePointer<CChar>(mutating: $0))
            }
        }
    }

    @objc func openUrl() {
        let alert = NSAlert()
        alert.messageText = "Open URL"
        alert.icon = appIcon
        alert.addButton(withTitle: "Ok")
        alert.addButton(withTitle: "Cancel")

        let input = NSTextField(frame: NSRect(x: 0, y: 0, width: 300, height: 24))
        input.placeholderString = "URL"
        alert.accessoryView = input

        DispatchQueue.main.asyncAfter(deadline: DispatchTime.now() + 0.1) {
            input.becomeFirstResponder()
        }

        if alert.runModal() == .alertFirstButtonReturn && input.stringValue.count > 0 {
            (NSApp as? Application)?.openFiles([input.stringValue])
        }
    }

    @objc func command(_ menuItem: NSMenuItem) {
        guard let menuConfig = getConfigFromMenu(menuItem: menuItem) else { return }
        menuConfig.command.withCString {
            (NSApp as? Application)?.queueCommand(UnsafeMutablePointer<CChar>(mutating: $0))
        }
    }

    @objc func url(_ menuItem: NSMenuItem) {
        guard let menuConfig = getConfigFromMenu(menuItem: menuItem),
              let url = URL(string: menuConfig.url ?? "") else { return }
        NSWorkspace.shared.open(url)
    }

    @objc func showFile(_ menuItem: NSMenuItem) {
        guard let menuConfig = getConfigFromMenu(menuItem: menuItem) else { return }
        let url = URL(fileURLWithPath: menuConfig.file ?? "")
        if FileManager.default.fileExists(atPath: url.path) {
            NSWorkspace.shared.activateFileViewerSelecting([url])
            return
        }

        alert(title: menuConfig.alertTitle1 ?? "", text: menuConfig.alertText1 ?? "")
    }

    func alert(title: String, text: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = text
        alert.icon = appIcon
        alert.addButton(withTitle: "Ok")
        alert.runModal()
    }

    func getConfigFromMenu(menuItem: NSMenuItem) -> Config? {
        for menuConfig in menuConfigs {
            for subConfig in menuConfig.configs ?? [] {
                if subConfig.menuItem == menuItem {
                    return subConfig
                }
            }
        }

        return nil
    }

    func register(_ selector: Selector, key: MenuKey) {
        for menuConfig in menuConfigs {
            for subConfig in menuConfig.configs ?? [] {
                if subConfig.commandSpecial == key {
                    subConfig.menuItem?.action = selector
                    return
                }
            }
        }
    }
}