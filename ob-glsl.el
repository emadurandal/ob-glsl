;;; ob-glsl.el --- GLSL Org Babel blocks with Canvas results -*- lexical-binding: t; -*-

;;; Commentary:

;; Execute GLSL fragment shaders from Org Babel.  File results are saved as
;; PNGs; blocks without :file display an animated Emacs Canvas result.

;;; Code:

(require 'cl-lib)
(require 'ob)
(require 'ob-glsl-module)
(require 'subr-x)

(define-error 'native-exception "Native ob-glsl error")

(defvar org-babel-default-header-args:glsl
  '((:results . "file link replace") (:exports . "results"))
  "Default arguments used when evaluating a GLSL source block.")

(defconst org-babel-header-args:glsl
  '((width . :any) (height . :any) (time . :any))
  "GLSL-specific Babel header arguments.")

(defconst ob-glsl--frame-period (/ 1.0 60.0))
(defconst ob-glsl--result-token "{GLSL Canvas}")

(cl-defstruct (ob-glsl--state (:constructor ob-glsl--make-state))
  buffer canvas renderer overlay timer started-at time-offset cleaning)

(defvar-local ob-glsl--pending-state nil)
(defvar-local ob-glsl--active-states nil)

(defun ob-glsl--parse-positive-integer (value name)
  "Parse positive integer VALUE for header argument NAME."
  (when value
    (let ((text (format "%s" value)))
      (unless (string-match-p "\\`[0-9]+\\'" text)
        (user-error "%s must be a positive integer, got %S" name value))
      (let ((number (string-to-number text)))
        (unless (> number 0)
          (user-error "%s must be greater than zero, got %S" name value))
        number))))

(defun ob-glsl--parse-time (value)
  "Parse VALUE as a floating-point time offset in seconds."
  (if (or (null value)
          (and (stringp value) (string-empty-p value)))
      0.0
    (let ((text (format "%s" value)))
      (unless (string-match-p
               "\\`[+-]?\\(?:[0-9]+\\(?:\\.[0-9]*\\)?\\|\\.[0-9]+\\)\\(?:[eE][+-]?[0-9]+\\)?\\'"
               text)
        (user-error ":time must be a number of seconds, got %S" value))
      (float (string-to-number text)))))

(defun ob-glsl--dimensions (params)
  "Return render dimensions derived from Babel PARAMS."
  (let ((width (ob-glsl--parse-positive-integer
                (cdr (assq :width params)) ":width"))
        (height (ob-glsl--parse-positive-integer
                 (cdr (assq :height params)) ":height")))
    (cond
     ((and width height))
     (width (setq height (round (/ (* width 3.0) 4.0))))
     (height (setq width (round (/ (* height 4.0) 3.0))))
     (t (setq width 400 height 300)))
    (cons width height)))

(defun org-babel-expand-body:glsl (body _params)
  "Expand GLSL BODY with the uniforms supplied by ob-glsl."
  (concat
   "#version 330 core\n"
   "out vec4 fragColor;\n"
   "uniform vec2 iResolution;\n"
   "uniform float iTime;\n"
   body))

(defun ob-glsl--cleanup (state)
  "Stop and release every resource owned by animation STATE."
  (when (and state (not (ob-glsl--state-cleaning state)))
    (setf (ob-glsl--state-cleaning state) t)
    (when-let* ((timer (ob-glsl--state-timer state)))
      (cancel-timer timer)
      (setf (ob-glsl--state-timer state) nil))
    (when-let* ((renderer (ob-glsl--state-renderer state)))
      (ignore-errors (ob-glsl-destroy-renderer renderer))
      (setf (ob-glsl--state-renderer state) nil))
    (when-let* ((overlay (ob-glsl--state-overlay state)))
      (overlay-put overlay 'ob-glsl-state nil)
      (delete-overlay overlay)
      (setf (ob-glsl--state-overlay state) nil))
    (when-let* ((buffer (ob-glsl--state-buffer state)))
      (when (buffer-live-p buffer)
        (with-current-buffer buffer
          (setq ob-glsl--active-states
                (delq state ob-glsl--active-states)))))
    (setf (ob-glsl--state-canvas state) nil)))

(defun ob-glsl--overlay-modified (overlay after _begin _end &optional _length)
  "Clean up the state on OVERLAY before a buffer modification.
AFTER is non-nil for the post-modification invocation."
  (unless after
    (when-let* ((state (overlay-get overlay 'ob-glsl-state)))
      (ob-glsl--cleanup state))))

(defun ob-glsl--after-change (_begin _end _old-length)
  "Release animation states whose result overlays were removed.
Deleting an entire evaporating overlay does not reliably invoke its
modification hook, so sweep detached overlays after buffer edits."
  (dolist (state (copy-sequence ob-glsl--active-states))
    (let ((overlay (ob-glsl--state-overlay state)))
      (when (or (not (overlayp overlay))
                (not (overlay-buffer overlay)))
        (ob-glsl--cleanup state)))))

(defun ob-glsl--cleanup-buffer ()
  "Release all pending and active GLSL renderers in the current buffer."
  (when ob-glsl--pending-state
    (ob-glsl--cleanup ob-glsl--pending-state)
    (setq ob-glsl--pending-state nil))
  (mapc #'ob-glsl--cleanup (copy-sequence ob-glsl--active-states))
  (setq ob-glsl--active-states nil))

(defun ob-glsl--cleanup-current-result (&rest _ignored)
  "Release any GLSL Canvas attached to the current Babel result."
  (when-let* ((result-position (org-babel-where-is-src-block-result)))
    (let ((result-line-end
           (save-excursion
             (goto-char result-position)
             (forward-line 1)
             (line-end-position))))
      (dolist (state (copy-sequence ob-glsl--active-states))
        (let ((overlay (ob-glsl--state-overlay state)))
          (when (and (overlayp overlay)
                     (eq (overlay-buffer overlay) (current-buffer))
                     (>= (overlay-start overlay) result-position)
                     (<= (overlay-end overlay) result-line-end))
            (ob-glsl--cleanup state)))))))

(defun ob-glsl--elapsed-time (state)
  "Return the current shader time for animation STATE."
  (+ (ob-glsl--state-time-offset state)
     (float-time (time-subtract nil (ob-glsl--state-started-at state)))))

(defun ob-glsl--timer-tick (state)
  "Publish and queue an asynchronous frame for animation STATE."
  (let ((buffer (ob-glsl--state-buffer state))
        (overlay (ob-glsl--state-overlay state)))
    (if (or (not (buffer-live-p buffer))
            (not (overlayp overlay))
            (not (overlay-buffer overlay)))
        (ob-glsl--cleanup state)
      (with-current-buffer buffer
        (condition-case error-data
            (when (ob-glsl-render-canvas
                   (ob-glsl--state-renderer state)
                   (ob-glsl--state-canvas state)
                   (ob-glsl--elapsed-time state))
              (canvas-refresh (ob-glsl--state-canvas state)))
          (error
           (ob-glsl--cleanup state)
           (message "ob-glsl animation stopped: %s"
                    (error-message-string error-data))))))))

(defun ob-glsl--result-overlay ()
  "Create an overlay covering the newly inserted GLSL result line."
  (let ((result-position (org-babel-where-is-src-block-result)))
    (unless result-position
      (error "Ob-glsl could not locate its inserted result"))
    (save-excursion
      (goto-char result-position)
      (forward-line 1)
      (unless (search-forward ob-glsl--result-token
                              (line-end-position) t)
        (error "Ob-glsl could not locate its result placeholder"))
      ;; Keep the placeholder in normal colon-prefixed Babel result syntax so
      ;; Org can find and replace it on the next execution.  The display
      ;; overlay covers the prefix as well as the token.
      (make-overlay (line-beginning-position)
                    (line-end-position) (current-buffer) nil t))))

(defun ob-glsl--install-pending-result ()
  "Install the Canvas prepared by the most recent GLSL execution."
  (when ob-glsl--pending-state
    (let ((state ob-glsl--pending-state))
      (setq ob-glsl--pending-state nil)
      (condition-case error-data
          (progn
            (org-babel-insert-result
             ob-glsl--result-token '("replace")
             (org-babel-get-src-block-info 'light) nil "glsl")
            (let ((overlay (ob-glsl--result-overlay)))
              (setf (ob-glsl--state-overlay state) overlay)
              (overlay-put overlay 'display (ob-glsl--state-canvas state))
              (overlay-put overlay 'evaporate t)
              (overlay-put overlay 'ob-glsl-state state)
              (overlay-put overlay 'modification-hooks
                           '(ob-glsl--overlay-modified))
              (push state ob-glsl--active-states)
              ;; Ensure the image spec has been realized before asking the
              ;; module for its Canvas storage.
              (redisplay)
              (ob-glsl-render-canvas-sync
               (ob-glsl--state-renderer state)
               (ob-glsl--state-canvas state)
               (ob-glsl--state-time-offset state))
              (canvas-refresh (ob-glsl--state-canvas state))
              (setf (ob-glsl--state-timer state)
                    (run-at-time ob-glsl--frame-period
                                 ob-glsl--frame-period
                                 #'ob-glsl--timer-tick state))))
        (error
         (ob-glsl--cleanup state)
         (signal (car error-data) (cdr error-data)))))))

(add-hook 'org-babel-after-execute-hook #'ob-glsl--install-pending-result)
(unless (advice-member-p #'ob-glsl--cleanup-current-result
                         'org-babel-insert-result)
  (advice-add 'org-babel-insert-result :before
              #'ob-glsl--cleanup-current-result))
(unless (advice-member-p #'ob-glsl--cleanup-current-result
                         'org-babel-remove-result)
  (advice-add 'org-babel-remove-result :before
              #'ob-glsl--cleanup-current-result))

(defun org-babel-execute:glsl (body params)
  "Execute a GLSL source block BODY according to PARAMS."
  (pcase-let* ((`(,width . ,height) (ob-glsl--dimensions params))
               (time-offset (ob-glsl--parse-time (cdr (assq :time params))))
               (shader-code (org-babel-expand-body:glsl body params))
               (file-value (cdr (assq :file params)))
               (output-file (and file-value
                                 (not (string-empty-p file-value))
                                 (org-babel-process-file-name file-value t))))
    (if output-file
        (ob-glsl-run shader-code width height output-file time-offset)
      (let ((result-params (cdr (assq :result-params params))))
        (unless (or (member "none" result-params)
                    (member "silent" result-params))
          (when ob-glsl--pending-state
            (ob-glsl--cleanup ob-glsl--pending-state))
          (add-hook 'kill-buffer-hook #'ob-glsl--cleanup-buffer nil t)
          (add-hook 'after-change-functions #'ob-glsl--after-change nil t)
          (setq ob-glsl--pending-state
                (ob-glsl--make-state
                 :buffer (current-buffer)
                 :canvas `(image :type canvas
                                 :id ,(gensym "ob-glsl-canvas-")
                                 :data-width ,width
                                 :data-height ,height)
                 :renderer (ob-glsl-create-renderer
                            shader-code width height)
                 :started-at (current-time)
                 :time-offset time-offset))))
      nil)))

(defun org-babel-prep-session:glsl (_session _params)
  "Report that GLSL source blocks do not support Babel sessions."
  (error "GLSL does not support sessions"))

(provide 'ob-glsl)

;;; ob-glsl.el ends here
