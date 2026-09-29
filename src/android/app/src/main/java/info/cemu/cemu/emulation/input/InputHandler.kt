package info.cemu.cemu.emulation.input

import android.view.KeyEvent
import android.view.MotionEvent
import info.cemu.cemu.common.android.inputevent.isFromPhysicalController
import info.cemu.cemu.nativeinterface.NativeInput
import kotlin.math.abs

object InputHandler {
    fun onKeyEvent(event: KeyEvent): Boolean {
        if (!event.isFromPhysicalController()) {
            return false
        }

        val device = event.device

        NativeInput.onControllerKey(
            device.descriptor,
            event.keyCode,
            event.action == KeyEvent.ACTION_DOWN
        )

        return true
    }

    fun onMotionEvent(event: MotionEvent): Boolean {
        if (!event.isFromPhysicalController()) {
            return false
        }

        val device = event.device
        val actionPointerIndex = event.actionIndex
        for (motionRange in device.motionRanges) {
            val axisValue = event.getAxisValue(motionRange.axis, actionPointerIndex)
            val axis = motionRange.axis
            NativeInput.onControllerAxis(device.descriptor, axis, axisValue)
        }

        return true
    }

    /**
     * Forwards only the axes that are back at rest (near zero), so a stick or trigger that was
     * held when input listening got disabled is released without letting new movement through.
     */
    fun onMotionEventReleasesOnly(event: MotionEvent) {
        if (!event.isFromPhysicalController()) {
            return
        }

        val device = event.device
        val actionPointerIndex = event.actionIndex
        for (motionRange in device.motionRanges) {
            val axisValue = event.getAxisValue(motionRange.axis, actionPointerIndex)
            if (abs(axisValue) <= REST_THRESHOLD) {
                NativeInput.onControllerAxis(device.descriptor, motionRange.axis, 0f)
            }
        }
    }

    private const val REST_THRESHOLD = 0.2f
}
