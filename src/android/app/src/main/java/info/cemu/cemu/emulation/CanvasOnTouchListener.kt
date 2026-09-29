package info.cemu.cemu.emulation

import android.annotation.SuppressLint
import android.view.MotionEvent
import android.view.View
import info.cemu.cemu.nativeinterface.NativeInput

class CanvasOnTouchListener(val isTV: Boolean) : View.OnTouchListener {
    private var currentPointerId: Int = -1
    private var isTouching = false
    private var lastX = 0
    private var lastY = 0

    /** Lifts the emulated touch if one is active. Safe to call at any time. */
    fun release(x: Int = lastX, y: Int = lastY) {
        currentPointerId = -1
        if (isTouching) {
            isTouching = false
            NativeInput.onTouchUp(x, y, isTV)
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouch(v: View, event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_CANCEL) {
            // The gesture was taken away (e.g. the system back gesture that opens the menu).
            // Lift the emulated touch, otherwise the game sees the touch screen held forever.
            release(event.getX(0).toInt(), event.getY(0).toInt())
            return true
        }
        val pointerIndex = event.actionIndex
        val pointerId = event.getPointerId(pointerIndex)
        if (currentPointerId != -1 && pointerId != currentPointerId) {
            return false
        }
        val x = event.getX(pointerIndex).toInt()
        val y = event.getY(pointerIndex).toInt()
        lastX = x
        lastY = y
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                isTouching = true
                NativeInput.onTouchDown(x, y, isTV)
                return true
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                release(x, y)
                return true
            }

            MotionEvent.ACTION_MOVE -> {
                NativeInput.onTouchMove(x, y, isTV)
                return true
            }
        }
        return false
    }
}