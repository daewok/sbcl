(defpackage #:sb-dump-linkage-info
  (:use #:cl)
  (:export #:*libdl-symbols*
           #:dump-to-file))

(in-package #:sb-dump-linkage-info)

(defparameter *libdl-symbols* '("dladdr" "dlclose" "dlerror" "dlopen" "dlsym"))

(defun dump-to-file (pn &key (make-undefined nil))
  (let ((ht (car sb-sys:*linkage-info*))
        (undefined-entries (cdr sb-sys:*linkage-info*))
        out)
    (loop
      :for key :being :the :hash-keys :in ht :using (hash-value idx)
      :for datap := (listp key)
      :for name := (if datap (first key) key)
      :for undefinedp := (not (null (or (member key undefined-entries :test #'equal)
                                        (member name make-undefined :test #'equal))))
      :do (push (cons idx (list name datap undefinedp)) out))
    (ensure-directories-exist pn)
    (with-open-file (s pn :direction :output :if-exists :supersede)
      (let ((*print-pretty* nil))
        (prin1 (mapcar #'cdr (sort out #'< :key #'car)) s))
      (terpri s)))
  pn)
