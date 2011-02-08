goog.provide('complete')

goog.require('goog.dom');
goog.require('goog.ui.AutoComplete.RichRemote');


/** @typedef {{path: string, path_highlight_ranges: Array.<Array.<number>>}} */
complete.JsonData;


/**
 * @param{complete.JsonData} data
 * @constructor
 */
complete.Entry = function(data) {
  /**
   * @type {complete.JsonData}
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

  // Use ["path"] instead of .path to prevent the compiler from renaming this.
  var path = this.data_["path"];
  var pathranges = this.data_["path_highlight_ranges"];

  // Create highlighted span. Assume that the ranges are sorted and
  // non-overlapping. TODO(thakis): This block of code is ridiculously long.
  var nodes = [];
  var textIndex = 0;
  for (var i = 0; i < pathranges.length; ++i) {
    var range = pathranges[i];
    var from = range[0];
    var to = range[1];
    if (textIndex < from)
      nodes.push(dom.createTextNode(path.substring(textIndex, from)));

    var span = dom.createDom('span', 'ac-highlighted');
    dom.appendChild(span, dom.createTextNode(path.substring(from, to + 1)));
    nodes.push(span);
    textIndex = to + 1;
  }
  if (textIndex < path.length)
    nodes.push(dom.createTextNode(path.substring(textIndex, path.length)));

  // Add highlighted fragment into a span.
  var pathNode = dom.createDom('span', 'complete-ac-path', nodes);
  dom.appendChild(node, pathNode);

  // TODO(thakis): Could be configurable; TextMate supports e.g. txmt://
  var macvimNode = dom.createDom('a', 'complete-ac-mvim');
  var url = 'mvim://open?url=file:///Users/thakis/src/chrome-git/src/';
  macvimNode.setAttribute('href', url + path);
  dom.appendChild(macvimNode, dom.createTextNode('Open in MacVim'));
  dom.appendChild(node, macvimNode);
}

/**
 * @inheritDoc
 */
complete.Entry.prototype.toString = function(target) {
  // Called to learn what to put into the input box if this is clicked. Called
  // right before |select()|.
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
 * @param{complete.JsonData} item
 * @return {complete.Entry}
 */
var filenames = function(item) {
  return new complete.Entry(item);
}

complete.setUp = function() {
  var input = goog.dom.getElement('txtInput');
  // TODO:
  // http://closure-library.googlecode.com/svn/trunk/closure/goog/demos/autocompleterichremote.html
  // (view source; use rich results.)
  // FIXME: be disposable
  var ac = new goog.ui.AutoComplete.RichRemote('http://localhost:8080', input);
  ac.getRenderer().setUseStandardHighlighting(false);

  // FIXME: would be nice to have the box open immediately somehow.
  //setTimeout(function() {
    ////ac.setActiveElement(ac.getInputHandler().getActiveElement());  // null??
    //ac.setToken(' ');

    ////ac.getInputHandler().update(true);
  //}, 800);
}

complete.setUp();

goog.exportSymbol('filenames', filenames);
