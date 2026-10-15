# Mask points and snapshot updates

In the preview, switch from **Layer** to **Mask** mode, then click a mask to select it.

- Double-click its contour to insert a point. The existing Bézier curve keeps its exact shape.
- Right-click a point and choose **Smooth Point (Bézier)** to create handles, or **Corner Point** to collapse them.
- Drag either handle to adjust the curve. Hold Shift to move a handle independently.
- Press Delete/Backspace or choose **Delete Point** from the point menu to remove it. A mask keeps at least three points.
- These edits support Undo and Redo. Layer mode does not delete mask points.

Right-click an inspector parameter or parameter tab to use **Update in Snapshot** and choose a target, or **Update in Selected Snapshot (name)**. A layer must already belong to the target snapshot; unavailable targets are disabled. Only the chosen values or category are updated. Other layers, inclusion flags, fade times, delays and easing stay as stored. The edit is undoable.

The About dialog and application icons use the supplied PINOL artwork in `assets/pinol.png`.
