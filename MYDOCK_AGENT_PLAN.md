# MyDock — Product Development and Engineering Plan

## 1. Project overview

MyDock is a Windows 11 taskbar replacement with a macOS-inspired dock experience and Windows Fluent visual styling.

This is a **fresh implementation**. Do not continue, refactor, copy or depend on any previous MyDock codebase. Treat previous prototypes as unrelated projects. Start with a clean architecture and repository.

The development process has two major phases:

1. **Phase 1 — Build and finalise the dock UI/UX and its application functionality.**
2. **Phase 2 — Integrate MyDock with Windows and implement safe native taskbar replacement.**

The user is involved at two approval gates only:
- Manual review and approval of the completed Phase 1 UI/UX.
- Manual testing and approval of the completed taskbar replacement.

You own routine engineering decisions, implementation, builds, debugging and verification. Do not ask the user to make technical decisions that you can reasonably make yourself.

## 2. Product direction

- Target OS: Windows 11.
- Language preference: C++.
- Preferred development environment: Visual Studio and MSBuild.
- Visual direction: macOS-inspired dock behaviour combined with Windows Fluent aesthetics.
- Start menu: retain and invoke the native Windows Start menu; do not build a custom Start menu.
- Architecture: fresh, modular implementation.
- UI framework and Windows composition technology: evaluate independently before committing. Verify support for transparent floating windows, composition, animation, input handling and later Windows shell integration.

The dock should be compact, floating, translucent and positioned at the bottom of the monitor. It must not look like a full-width Windows taskbar.

## 3. Development phases and approval gates

| Stage | Objective | User involvement |
|---|---|---|
| Phase 0 | Technical evaluation and project foundation | None |
| Phase 1 | Functional dock UI/UX | User review |
| Phase 1.5 | Apply design feedback until approved | User review as needed |
| Phase 2 | Windows shell integration and taskbar replacement | No routine involvement |
| Phase 3 | Final reliability and safety testing | User tests and approves |

**Do not begin Phase 2 until the user explicitly approves Phase 1.**

During Phase 1, the native Windows taskbar must remain fully functional and untouched. Do not hide, suppress or modify it.

---

# Phase 0 — Technical foundation

Before implementing the dock:

1. Evaluate suitable UI frameworks and Windows APIs.
2. Verify that the chosen technology supports the required transparent, content-sized floating window and composition effects.
3. Evaluate compatibility with C++, Visual Studio and MSBuild.
4. Design a modular architecture with clear separation between presentation, dock logic, application/window tracking and Windows integration.
5. Establish the new repository, project structure, build configuration and Git baseline.
6. Build and run a minimal application.

Consider distinct responsibilities for:
- Dock window and presentation.
- Dock layout and animation.
- Pinned application management and persistence.
- Application discovery, identity and launching.
- Window tracking, grouping and activation.
- Icon retrieval and resource lifetime.
- Settings and configuration.
- Windows shell integration.

These are responsibilities, not mandatory class names. Choose an architecture appropriate to the selected framework. Avoid overengineering, unnecessary dependencies and premature abstractions.

Do not implement taskbar suppression in this phase.

# Phase 1 — Functional dock UI/UX

The outcome must be a working dock, not a static mock-up. Implement its real application behaviour while leaving the native Windows taskbar unchanged.

## 1. Overall layout

The dock consists of two primary sections, arranged horizontally.

### A. Fixed system-information section — left

Contains:

**Clock and date**
- Display time in 12-hour format.
- Display the date underneath.
- Update automatically.

**PC/monitor icon**
- Opens the appropriate Windows Action Center / Quick Settings entry point.
- Does not participate in magnification.

**Settings icon**
- Opens MyDock settings.
- Does not participate in magnification.

This section must retain stable dimensions, alignment and position. It must never magnify or shift when application icons are hovered.

A vertical divider separates this section from the application area.

### B. Application section — centre/right

Contains two logical groups, in this order:
1. Pinned applications.
2. Non-pinned running applications.

There must be **no visible divider between these two application groups**. They should appear as one continuous row of icons.

#### Pinned applications
- Always displayed.
- Retain their configured order.
- Remain visible when not running.
- Launch when clicked if not already running.
- Show a running-window indicator when active.

#### Non-pinned applications
- Display only currently running applications.
- Disappear when no longer running.
- Appear after the pinned applications.
- Must not duplicate an application already represented in the pinned group.

Group windows by application identity, not by window title. Multiple windows from the same application should be represented by one icon.

## 2. Window-count indicators

Show small horizontal lines beneath application icons to represent the number of open windows.

| Application state | Indicator |
|---|---|
| Pinned but not running | No line |
| Running with one window | One line |
| Running with two windows | Two lines |
| Running with three windows | Three lines |
| Running with more than three windows | Three lines maximum |

Requirements:
- Centre the indicators beneath their respective icons.
- Use subtle, consistent styling.
- Update dynamically as windows open and close.
- Keep indicators distinguishable during magnification.
- Show indicators for non-pinned running applications too.

## 3. Magnification and dynamic spacing

Implement a fluid, macOS-style magnification effect:
- The icon closest to the pointer receives the greatest magnification.
- Neighbouring icons receive progressively less magnification according to distance.
- Preserve a natural, smooth visual character.
- Dynamically move icons apart as they magnify.
- Icons must never overlap.
- Expand the application section horizontally as needed.
- Smoothly restore normal positions and sizes when the pointer leaves.

The fixed system-information section must not move or magnify. Do not rely on a static gap that causes icons to collide when scaled.

## 4. Individual icon appearance

Application icons appear directly on the main dock surface:
- No individual icon backgrounds.
- No individual borders or rectangular/rounded tiles.
- No highlighted boxes or hover backgrounds.
- No unnecessary icon shadows or outlines.
- Only the icons themselves respond to pointer hover through magnification.

The main dock surface retains its own visual styling.

## 5. Hover behaviour

When the pointer hovers over an application:
- Apply the magnification behaviour.
- Display the application name in a tooltip after a short delay.
- Do not show an individual tile or highlighted background.

Tooltips should be unobtrusive and consistent with the overall Windows 11 visual language.

## 6. Right-click and window previews

Right-clicking an application icon opens a **separate window-preview panel**. Do not use a conventional context menu as the primary right-click interaction.

The panel should:
- Display all open windows belonging to that application.
- Show a visual thumbnail/preview for each window where supported.
- Display the corresponding window title.
- Allow the user to select a window and activate it.
- Handle minimized windows appropriately.
- Appear near the dock without unnecessarily obscuring it.
- Dismiss when the user clicks outside or selects a window.

The preview panel is a separate visual component with its own lifecycle and positioning. Do not expand the dock itself to contain the previews.

## 7. Auto-hide

Auto-hide is configurable in MyDock settings.

When enabled:
- The dock retracts when not in use.
- A narrow activation zone at the physical bottom edge reveals it.
- The dock appears smoothly.
- It retracts after the pointer leaves the dock and activation zone, with a sensible delay.
- Movement between icons must not trigger accidental hiding.
- Avoid flicker during transitions.

When disabled, the dock remains visible.

The activation zone must not appear as a permanently visible full-width bar. During Phase 1, auto-hide must not suppress or alter the native Windows taskbar.

## 8. Dock appearance, size and positioning

The dock should:
- Be a compact floating element, horizontally centred at the bottom of the monitor.
- Have content-driven dimensions rather than full-screen or full-width bounds.
- Expand horizontally when magnification requires additional space.
- Maintain consistent internal padding and margins.
- Use a translucent, glass-like main surface.
- Have subtle rounded corners and a refined border/highlight.
- Allow desktop wallpaper to remain partially visible through the surface.
- Have completely transparent surroundings outside its actual visible bounds.

Treat the native application window and the visible dock surface as separate composition/layout concerns. Do not solve a positioning issue merely by making an oversized window transparent. Do not create a full-screen overlay or taskbar-like full-width bar.

## 9. Phase 1 functionality

Implement actual functionality:
- Pinned application discovery, ordering and persistence.
- Application launching.
- Running application detection.
- Application identity and grouping.
- Window activation and restoration.
- Multiple-window tracking.
- Separate window-preview panel.
- Window-count indicators.
- Clock and date.
- Settings entry point.
- Action Center / Quick Settings entry point.
- Configurable auto-hide.
- Magnification and dynamic spacing.

The native Windows taskbar must remain fully functional throughout Phase 1.

## 10. Phase 1 acceptance criteria

Do not request the user's manual review until all applicable criteria below have been implemented and verified as far as the environment permits:

1. The dock has the intended two-section layout.
2. The system-information section remains fixed.
3. Pinned and non-pinned applications appear in the correct order, without a visible divider between those groups.
4. Window-count indicators work correctly.
5. Magnification works without icon overlap.
6. Individual icon tiles and hover backgrounds are absent.
7. Tooltips work.
8. Right-click opens a separate window-preview panel.
9. Window selection activates the correct window.
10. Auto-hide works when enabled and stays off when configured off.
11. The dock has compact dimensions and the intended translucent appearance.
12. The dock does not interfere with the native Windows taskbar.
13. The application builds and runs successfully.

Do not claim a visual or runtime behaviour has been verified if you have only inspected the source code.

# Phase 1.5 — User review and design refinement

When Phase 1 acceptance criteria are met:

1. Present the working application and a concise summary of implemented behaviour.
2. Ask the user to test it manually.
3. Receive design and interaction feedback.
4. Implement the requested refinements.
5. Rebuild and verify the changes.
6. Repeat until the user explicitly approves the UI/UX.

The user should not need to understand or edit the implementation. Keep requests for manual testing specific and focused.

**Do not begin Phase 2 without explicit user approval.**

Once approved, treat the Phase 1 UI/UX as the design baseline. Avoid unnecessary redesign during Phase 2.

# Phase 2 — Windows integration and native taskbar replacement

Only begin this phase after explicit Phase 1 approval.

The goal is to integrate MyDock with the Windows shell and provide the functionality required for a reliable taskbar replacement while preserving the approved dock UI/UX.

Evaluate and implement the functionality required for the agreed scope, potentially including:
- Native taskbar suppression and restoration.
- Explorer/taskbar lifecycle management.
- System tray icon discovery and interaction.
- Network, volume and battery access.
- Clock and calendar integration.
- Notification access.
- Native Start menu invocation.
- Windows Search access.
- Task View and virtual desktop integration.
- Multi-monitor support.
- Windows shell integration.
- Keyboard shortcut compatibility.
- Startup and shutdown behaviour.

The Start menu remains native Windows functionality. Do not implement a custom Start menu.

## Taskbar safety requirements

This phase is safety-critical:
- Never kill or patch Explorer.
- Never inject into or modify explorer.exe.
- Do not require administrator privileges without clear justification.
- Make native taskbar suppression explicitly opt-in.
- Implement reliable restoration mechanisms.
- Handle Explorer restarts and taskbar HWND recreation.
- Handle normal application shutdown and known failure paths.
- Provide an accessible recovery mechanism.
- Account for multiple monitors and Windows taskbar settings.
- Preserve Windows keyboard shortcuts and shell functionality wherever possible.
- Do not assume hiding the taskbar is equivalent to replacing its functionality.

Develop and verify recovery mechanisms before enabling suppression.

You may implement supporting code and safe tests, but **never change the user's live taskbar state or perform a live suppression test without explicit approval**. Ask the user at the appropriate testing gate.

Implement shell integration incrementally. Do not activate suppression just because the integration code has been written.

# Phase 3 — Reliability and final user testing

Before requesting final testing, verify:
- Startup and shutdown.
- Explorer restart recovery.
- Native taskbar restoration.
- Application crash and recovery behaviour.
- Sleep and wake behaviour.
- Multi-monitor behaviour.
- System tray interactions.
- Window switching and application grouping.
- Auto-hide.
- Keyboard shortcuts.
- Startup configuration and settings persistence.

The user will manually test whether MyDock can safely replace the native taskbar. Investigate and resolve issues found before considering the product complete.

Do not declare the product finished merely because it builds.

# Engineering workflow and guardrails

## Before implementation
- Inspect the repository and Git state.
- Preserve existing user work.
- Understand affected components.
- Plan the smallest coherent change.

## During implementation
- Make changes directly in the repository.
- Build frequently.
- Fix errors before proceeding.
- Avoid unnecessary dependencies and unrelated changes.
- Follow modern C++ and Windows development conventions.
- Separate UI, application logic and platform integration.
- Keep UI updates on the correct thread.
- Handle resource ownership and cleanup.
- Avoid leaving the repository in a broken intermediate state.

## Verification
- Build the actual Visual Studio solution/project.
- Run relevant automated checks.
- Verify runtime behaviour where possible.
- Distinguish automated verification from manual testing.
- Never claim a build or test succeeded unless it was actually executed.

## Git safety
- Inspect changes before modifying files.
- Preserve user work.
- Review diffs after coherent changes.
- Do not run destructive Git commands without permission.
- Do not commit unless authorised.

## Reporting
Provide concise reports at meaningful checkpoints:
- Work completed.
- Files changed.
- Build and test results.
- Known limitations.
- Manual testing required.
- Next stage.

Avoid lengthy explanations of implementation details unless requested.

# First assignment

Start with Phase 0:

1. Establish the fresh project and repository foundation.
2. Evaluate the appropriate Windows UI and composition technology.
3. Explain the important technology choices and proposed architecture briefly.
4. Build and run a minimal application.
5. Prepare a short, actionable breakdown of Phase 1.

Then proceed with Phase 1 implementation.

Do not ask the user to design class structures, choose implementation details or debug Windows APIs. Make reasonable engineering decisions independently.

The user's involvement is reserved for product feedback at the Phase 1 approval gate and safety testing at the end of Phase 2.

**Primary objective: deliver a polished, fully functional dock first. Only after its UI/UX is approved should you turn it into a native Windows taskbar replacement.**
