goog.provide('complete')

goog.require('goog.dom');
goog.require('goog.ui.AutoComplete.RichRemote');

/**
 * @param{Object} data
 * @constructor
 */
complete.Entry = function(data) {
  /**
   * @type {Object}
   * @private
   */
  this.data_ = data;
}

/**
 * Called by closure to render each completion entry.
 * @param{Element} node The element to render output to.
 * @param{string} token The current user input.
 */
complete.Entry.prototype.render = function(node, token) {
  var dom = goog.dom.getDomHelper(node);
  var pathNode = dom.createDom("span", 'complete-ac-path');
  dom.appendChild(pathNode, dom.createTextNode(this.data_.path));

  dom.appendChild(node, pathNode);
}

/**
 * @inheritdocs
 * @override
 */
complete.Entry.prototype.toString = function(target) {
  // Called to learn what to put into the input box if this is clicked.
  return this.data_.path;
}

/**
 * Called by closure to if the item is selected by the user.
 * @param{Element} target The input element the completion popup belongs to.
 */
complete.Entry.prototype.select = function(target) {
  // Open the selected file at cs.chromium.org for now.
  var url = 'http://codesearch.google.com/codesearch/p?#OAMlx_jo-ck/src/';
  url += this.data_.path + '&exact_package=chromium';
  window.location = url;
}

// FIXME(thakis): export
/**
 * @param{Object} item
 */
filenames = function(item) {
  return new complete.Entry(item);
}

complete.setUp = function() {
  var input = goog.dom.getElement('txtInput');
  // TODO:
  // http://closure-library.googlecode.com/svn/trunk/closure/goog/demos/autocompleterichremote.html
  // (view source; use rich results.)
  // FIXME: be disposable
  var ac = new goog.ui.AutoComplete.RichRemote('http://hummer.mtv:8080', input);
  ac.getRenderer().setUseStandardHighlighting(false);

  // FIXME: would be nice to have the box open immediately somehow.
  //setTimeout(function() {
    ////ac.setActiveElement(ac.getInputHandler().getActiveElement());  // null??
    //ac.setToken(' ');

    ////ac.getInputHandler().update(true);
  //}, 800);
}

complete.setUp();

goog.exportSymbol('complete.setUp', complete.setUp);
