//
//  MachineDocument.swift
//  Clock Signal
//
//  Created by Thomas Harte on 04/01/2016.
//  Copyright © 2016 Thomas Harte. All rights reserved.
//

import Cocoa
import AudioToolbox

class MachineDocument:
	NSDocument,
	NSWindowDelegate,
	CSMachineDelegate,
	CSOpenGLViewDelegate,
	CSOpenGLViewResponderDelegate,
	CSBestEffortUpdaterDelegate,
	CSAudioQueueDelegate
{
	lazy var actionLock = NSLock()
	lazy var drawLock = NSLock()
	var machine: CSMachine!
	var name: String! {
		get {
			return nil
		}
	}

	func aspectRatio() -> NSSize {
		return NSSize(width: 4.0, height: 3.0)
	}

	@IBOutlet weak var openGLView: CSOpenGLView! {
		didSet {
			openGLView.delegate = self
			openGLView.responderDelegate = self
		}
	}

	@IBOutlet var optionsPanel: MachinePanel!
	@IBAction func showOptions(_ sender: AnyObject!) {
		optionsPanel?.setIsVisible(true)
	}

	fileprivate var audioQueue: CSAudioQueue! = nil
	fileprivate lazy var bestEffortUpdater: CSBestEffortUpdater = {
		let updater = CSBestEffortUpdater()
		updater.delegate = self
		return updater
	}()

	override var windowNibName: String? {
		return "MachineDocument"
	}

	override func windowControllerDidLoadNib(_ aController: NSWindowController) {
		super.windowControllerDidLoadNib(aController)

		// establish the output aspect ratio and audio
		let displayAspectRatio = self.aspectRatio()
		aController.window?.contentAspectRatio = displayAspectRatio
		openGLView.perform(glContext: {
			self.machine.setView(self.openGLView, aspectRatio: Float(displayAspectRatio.width / displayAspectRatio.height))
		})

		setupClockRate()
		self.machine.delegate = self
		establishStoredOptions()
	}

	func machineDidChangeClockRate(_ machine: CSMachine!) {
		setupClockRate()
	}

	func machineDidChangeClockIsUnlimited(_ machine: CSMachine!) {
		self.bestEffortUpdater.runAsUnlimited = machine.clockIsUnlimited
	}

	fileprivate func setupClockRate() {
		// establish and provide the audio queue, taking advice as to an appropriate sampling rate
		let maximumSamplingRate = CSAudioQueue.preferredSamplingRate()
		let selectedSamplingRate = self.machine.idealSamplingRate(from: NSRange(location: 0, length: NSInteger(maximumSamplingRate)))
		if selectedSamplingRate > 0 {
			audioQueue = CSAudioQueue(samplingRate: Float64(selectedSamplingRate))
			audioQueue.delegate = self
			self.machine.audioQueue = self.audioQueue
			self.machine.setAudioSamplingRate(selectedSamplingRate, bufferSize:audioQueue.bufferSize / 2)
		}

		self.bestEffortUpdater.clockRate = self.machine.clockRate
	}

	override func close() {
		actionLock.lock()
		drawLock.lock()
		openGLView.invalidate()
		openGLView.openGLContext!.makeCurrentContext()
		actionLock.unlock()
		drawLock.unlock()

		super.close()
	}

	// MARK: configuring
	func configureAs(_ analysis: CSStaticAnalyser) {
		if let machine = analysis.newMachine() {
			self.machine = machine
		}
		analysis.apply(to: self.machine)

		if let optionsPanelNibName = analysis.optionsPanelNibName {
			Bundle.main.loadNibNamed(optionsPanelNibName, owner: self, topLevelObjects: nil)
			self.optionsPanel.machine = self.machine
			showOptions(self)
		}
	}

	override func read(from url: URL, ofType typeName: String) throws {
		if let analyser = CSStaticAnalyser(fileAt: url) {
			self.displayName = analyser.displayName
			self.configureAs(analyser)
		}
	}

	// MARK: the pasteboard
	func paste(_ sender: AnyObject!) {
		let pasteboard = NSPasteboard.general()
		if let string = pasteboard.string(forType: NSPasteboardTypeString) {
			self.machine.paste(string)
		}
	}

	// MARK: CSBestEffortUpdaterDelegate
	final func bestEffortUpdater(_ bestEffortUpdater: CSBestEffortUpdater!, runForCycles cycles: UInt, didSkipPreviousUpdate: Bool) {
		runForNumberOfCycles(Int32(cycles))
	}

	func runForNumberOfCycles(_ numberOfCycles: Int32) {
		let cyclesToRunFor = min(numberOfCycles, Int32(bestEffortUpdater.clockRate / 10))
		if actionLock.try() {
			self.machine.runForNumber(ofCycles: cyclesToRunFor)
			actionLock.unlock()
		}
	}

	// MARK: Utilities for children
	func dataForResource(_ name : String, ofType type: String, inDirectory directory: String) -> Data? {
		if let path = Bundle.main.path(forResource: name, ofType: type, inDirectory: directory) {
			return (try? Data(contentsOf: URL(fileURLWithPath: path)))
		}

		return nil
	}

	// MARK: CSAudioQueueDelegate
	final func audioQueueDidCompleteBuffer(_ audioQueue: CSAudioQueue) {
		bestEffortUpdater.update()
	}

	// MARK: CSOpenGLViewDelegate
	final func openGLView(_ view: CSOpenGLView, drawViewOnlyIfDirty onlyIfDirty: Bool) {
		bestEffortUpdater.update()
		if drawLock.try() {
			self.machine.drawView(forPixelSize: view.backingSize, onlyIfDirty: onlyIfDirty)
			drawLock.unlock()
		}
	}

	// MARK: NSDocument overrides
	override func data(ofType typeName: String) throws -> Data {
		throw NSError(domain: NSOSStatusErrorDomain, code: unimpErr, userInfo: nil)
	}

	// MARK: Key forwarding
	fileprivate func withKeyboardMachine(_ action: (CSKeyboardMachine) -> ()) {
		if let keyboardMachine = self.machine as? CSKeyboardMachine {
			action(keyboardMachine)
		}
	}

	func windowDidResignKey(_ notification: Notification) {
		self.withKeyboardMachine { $0.clearAllKeys() }
	}

	func keyDown(_ event: NSEvent) {
		self.withKeyboardMachine { $0.setKey(event.keyCode, isPressed: true) }
	}

	func keyUp(_ event: NSEvent) {
		self.withKeyboardMachine { $0.setKey(event.keyCode, isPressed: false) }
	}

	func flagsChanged(_ newModifiers: NSEvent) {
		self.withKeyboardMachine {
			$0.setKey(VK_Shift, isPressed: newModifiers.modifierFlags.contains(.shift))
			$0.setKey(VK_Control, isPressed: newModifiers.modifierFlags.contains(.control))
			$0.setKey(VK_Command, isPressed: newModifiers.modifierFlags.contains(.command))
			$0.setKey(VK_Option, isPressed: newModifiers.modifierFlags.contains(.option))
		}
	}

	// MARK: IBActions
	final func prefixedUserDefaultsKey(_ key: String) -> String {
		return "\(self.name).\(key)"
	}
	var fastLoadingUserDefaultsKey: String {
		get {
			return prefixedUserDefaultsKey("fastLoading")
		}
	}

	@IBOutlet var fastLoadingButton: NSButton?
	@IBAction func setFastLoading(_ sender: NSButton!) {
		if let fastLoadingMachine = machine as? CSFastLoading {
			let useFastLoadingHack = sender.state == NSOnState
			fastLoadingMachine.useFastLoadingHack = useFastLoadingHack
			UserDefaults.standard.set(useFastLoadingHack, forKey: fastLoadingUserDefaultsKey)
		}
	}

	func establishStoredOptions() {
		let standardUserDefaults = UserDefaults.standard
		standardUserDefaults.register(defaults: [
			fastLoadingUserDefaultsKey: true
		])

		if let fastLoadingMachine = machine as? CSFastLoading {
			let useFastLoadingHack = standardUserDefaults.bool(forKey: self.fastLoadingUserDefaultsKey)
			fastLoadingMachine.useFastLoadingHack = useFastLoadingHack
			self.fastLoadingButton?.state = useFastLoadingHack ? NSOnState : NSOffState
		}
	}
}
